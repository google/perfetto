--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

INCLUDE PERFETTO MODULE linux.cpu.frequency;

-- Returns the timestamp of the start of the partition that contains the |ts|.
CREATE PERFETTO FUNCTION _partition_start(ts INT, size INT) RETURNS INT AS
-- Division of two ints would result in floor(ts/size).
SELECT ($ts/$size)*$size;

-- Returns the number of partitions required to cover all of the trace
-- timestamps.
CREATE PERFETTO FUNCTION _partition_count(size INT) RETURNS INT AS
SELECT
    (_partition_start(TRACE_END(), $size) -
    _partition_start(TRACE_START(), $size))/$size + 1;

-- Returns a table of partitions with first partition containing the
-- TRACE_START() and last one containing TRACE_END().
CREATE PERFETTO FUNCTION _partitions(size INT)
RETURNS TABLE (ts INT, ts_end INT) AS
WITH no_ends AS (
SELECT
    _partition_start(TRACE_START(), $size) + (id * $size) AS ts
-- We are using the sched table for source of ids. If the table is too small
-- for specified size, the results would be invalid none the less.
FROM sched
LIMIT _partition_count($size))
SELECT ts, ts + $size AS ts_end FROM no_ends;

-- Partitions any |intervals| table with partitions defined in the |partitions|
-- table.
CREATE PERFETTO MACRO _interval_partitions(
  -- Requires |ts| and |ts_end| columns.
  partitions TableOrSubquery,
  -- Requires |ts| and |ts_end| column.
  intervals TableOrSubquery
) RETURNS TableOrSubquery AS (
SELECT
  p.ts AS partition_ts,
  IIF(i.ts_end < p.ts_end, i.ts_end, p.ts_end) AS ts_end,
  IIF(i.ts < p.ts, p.ts, i.ts) AS ts
FROM $intervals i
JOIN $partitions p
ON (p.ts <= i.ts AND i.ts < p.ts_end));

-- Returns a table of utilization per given period.
-- Utilization is calculated as sum of average utilization of each CPU in each
-- period, which is defined as a multiply of |interval|. For this reason
-- first and last period might have lower then real utilization.
CREATE PERFETTO MACRO _cpu_avg_utilization_per_period(
  -- Length of the period on which utilization should be averaged.
  interval Expr,
  -- Either sched table or its filtered down version.
  sched_table TableOrSubquery
)
-- The returned table has the schema (ts UINT32, utilization DOUBLE,
-- unnormalized_utilization DOUBLE).
RETURNS TableOrSubquery AS (
SELECT
  partition_ts AS ts,
  SUM(ts_end - ts)/(cast_double!($interval) * (SELECT MAX(cpu) + 1 FROM sched)) AS utilization,
  SUM(ts_end - ts)/cast_double!($interval) AS unnormalized_utilization
FROM _interval_partitions!(_partitions($interval), $sched_table)
GROUP BY 1);

CREATE PERFETTO VIEW _cpu_freq_for_metrics AS
SELECT
    id,
    ts,
    dur,
    cpu,
    ucpu,
    freq
FROM cpu_frequency_counters
WHERE freq IS NOT NULL;

CREATE PERFETTO VIEW _sched_without_id AS
SELECT ts, dur, utid, ucpu
FROM sched
WHERE utid != 0 AND dur != -1;

CREATE VIRTUAL TABLE _cpu_freq_per_thread_span_join
USING SPAN_LEFT_JOIN(
    _sched_without_id PARTITIONED ucpu,
    _cpu_freq_for_metrics PARTITIONED ucpu);

CREATE PERFETTO TABLE _cpu_freq_per_thread_no_id
AS
SELECT *
FROM _cpu_freq_per_thread_span_join;

CREATE PERFETTO VIEW _cpu_freq_per_thread AS
SELECT
  _auto_id AS id,
  ts,
  dur,
  ucpu,
  cpu,
  utid,
  freq,
  id AS counter_id
FROM _cpu_freq_per_thread_no_id;

