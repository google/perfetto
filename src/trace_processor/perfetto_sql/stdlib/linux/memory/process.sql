--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

INCLUDE PERFETTO MODULE linux.memory.general;

-- All memory counters tables.

CREATE PERFETTO VIEW _anon_rss AS
SELECT ts, dur, upid, value AS anon_rss_val
FROM _all_counters_per_process
WHERE
  name = 'mem.rss.anon';

CREATE PERFETTO VIEW _file_rss AS
SELECT ts, dur, upid, value AS file_rss_val
FROM _all_counters_per_process
WHERE
  name = 'mem.rss.file';

CREATE PERFETTO VIEW _shmem_rss AS
SELECT ts, dur, upid, value AS shmem_rss_val
FROM _all_counters_per_process
WHERE
  name = 'mem.rss.shmem';

CREATE PERFETTO VIEW _swap AS
SELECT ts, dur, upid, value AS swap_val
FROM _all_counters_per_process
WHERE
  name = 'mem.swap';

-- Used by the android_process_mem metric.
CREATE VIRTUAL TABLE _anon_swap_sj USING SPAN_OUTER_JOIN(
  _anon_rss PARTITIONED upid,
  _swap PARTITIONED upid);

-- Each process's anon RSS, file RSS, shmem RSS and swap over time, split at
-- every change of any of them. No counter overlaps itself within a process, so
-- MAX picks the one value active in each segment.
--
-- A counter value with `dur = 0` gives a `dur = 0` segment. There it wins over
-- the value of the same counter starting at the same `ts`, so `*_point` columns
-- keep it apart.
CREATE PERFETTO TABLE _memory_rss_and_swap_per_process_flattened AS
FROM (
  SELECT
    ts,
    dur,
    upid,
    iif(name = 'mem.rss.anon' AND dur > 0, cast_int!(value), NULL) AS anon_rss,
    iif(name = 'mem.rss.anon' AND dur = 0, cast_int!(value), NULL) AS anon_rss_point,
    iif(name = 'mem.rss.file' AND dur > 0, cast_int!(value), NULL) AS file_rss,
    iif(name = 'mem.rss.file' AND dur = 0, cast_int!(value), NULL) AS file_rss_point,
    iif(name = 'mem.rss.shmem' AND dur > 0, cast_int!(value), NULL) AS shmem_rss,
    iif(name = 'mem.rss.shmem' AND dur = 0, cast_int!(value), NULL) AS shmem_rss_point,
    iif(name = 'mem.swap' AND dur > 0, cast_int!(value), NULL) AS swap,
    iif(name = 'mem.swap' AND dur = 0, cast_int!(value), NULL) AS swap_point
  FROM _all_counters_per_process
  WHERE
    name IN ('mem.rss.anon', 'mem.rss.file', 'mem.rss.shmem', 'mem.swap')
    AND dur >= 0
)
|> INTERVAL FLATTEN PER upid
  AGGREGATE MAX(anon_rss) AS anon_rss,
  MAX(anon_rss_point) AS anon_rss_point,
  MAX(file_rss) AS file_rss,
  MAX(file_rss_point) AS file_rss_point,
  MAX(shmem_rss) AS shmem_rss,
  MAX(shmem_rss_point) AS shmem_rss_point,
  MAX(swap) AS swap,
  MAX(swap_point) AS swap_point
|> ORDER BY upid, ts, dur;

CREATE PERFETTO TABLE _memory_rss_and_swap_per_process_table AS
SELECT
  ts,
  dur,
  upid,
  coalesce(anon_rss_point, anon_rss) AS anon_rss,
  coalesce(file_rss_point, file_rss) AS file_rss,
  coalesce(shmem_rss_point, shmem_rss) AS shmem_rss,
  coalesce(swap_point, swap) AS swap
FROM _memory_rss_and_swap_per_process_flattened;

-- Memory metrics timeline for each process.
CREATE PERFETTO VIEW memory_rss_and_swap_per_process(
  -- Timestamp
  ts TIMESTAMP,
  -- Duration
  dur DURATION,
  -- Upid of the process
  upid JOINID(process.id),
  -- Pid of the process
  pid LONG,
  -- Name of the process
  process_name STRING,
  -- Anon RSS counter value
  anon_rss LONG,
  -- File RSS counter value
  file_rss LONG,
  -- Shared memory RSS counter value
  shmem_rss LONG,
  -- Total RSS value. Sum of `anon_rss`, `file_rss` and `shmem_rss`. Returns
  -- value even if one of the values is NULL.
  rss LONG,
  -- Swap counter value
  swap LONG,
  -- Sum or `anon_rss` and `swap`. Returns value even if one of the values is
  -- NULL.
  anon_rss_and_swap LONG,
  -- Sum or `rss` and `swap`. Returns value even if one of the values is NULL.
  rss_and_swap LONG
)
AS
SELECT
  ts,
  dur,
  upid,
  pid,
  name AS process_name,
  anon_rss,
  file_rss,
  shmem_rss,
  -- We do COALESCE only on `shmem_rss` and `swap`, as it can be expected all
  -- process start to emit anon rss and file rss events (you'll need to at
  -- least read code and have some memory to work with) - so the NULLs are real
  --  values. But it is possible that you will never swap or never use shmem,
  -- so those values are expected to often be NULLs, which shouldn't propagate
  -- into the values like `anon_and_swap` or `rss`.
  file_rss + anon_rss + coalesce(shmem_rss, 0) AS rss,
  swap,
  anon_rss + coalesce(swap, 0) AS anon_rss_and_swap,
  anon_rss + file_rss + coalesce(shmem_rss, 0) + coalesce(swap, 0) AS rss_and_swap
FROM _memory_rss_and_swap_per_process_table
JOIN process USING (upid);
