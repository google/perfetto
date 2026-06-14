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

-- NOTE (psqlnext): the `*_in_interval` constructs are parameterized by a scalar
-- window, so they are pipeline-valued macros (`RETURNS Pipeline`) clipping the
-- relevant relation with `_interval_intersect_single!($ts, $dur, …)`.

INCLUDE PERFETTO MODULE linux.cpu.utilization.general;

INCLUDE PERFETTO MODULE linux.cpu.utilization.thread_cpu;

INCLUDE PERFETTO MODULE time.conversion;

INCLUDE PERFETTO MODULE intervals.intersect;

-- Returns a table of thread utilization per given period.
-- Utilization is calculated as sum of average utilization of each CPU in each
-- period, which is defined as a multiply of |interval|. For this reason
-- first and last period might have lower then real utilization.
CREATE PERFETTO FUNCTION cpu_thread_utilization_per_period(
  -- Length of the period on which utilization should be averaged.
  interval LONG,
  -- Utid of the thread.
  utid JOINID(thread.id)
)
RETURNS TABLE(
  -- Timestamp of start of a second.
  ts TIMESTAMP,
  -- Sum of average utilization over period.
  -- Note: as the data is normalized, the values will be in the
  -- [0, 1] range.
  utilization DOUBLE,
  -- Sum of average utilization over all CPUs over period.
  -- Note: as the data is unnormalized, the values will be in the
  -- [0, cpu_count] range.
  unnormalized_utilization DOUBLE
)
AS
WITH sched_for_utid AS (SELECT ts, ts_end, utid FROM sched WHERE utid = $utid)
SELECT * FROM _cpu_avg_utilization_per_period!($interval, sched_for_utid);

-- Returns a table of thread utilization per second.
-- Utilization is calculated as sum of average utilization of each CPU in each
-- period, which is defined as a multiply of |interval|. For this reason
-- first and last period might have lower then real utilization.
CREATE PERFETTO FUNCTION cpu_thread_utilization_per_second(
  -- Utid of the thread.
  utid JOINID(thread.id)
)
RETURNS TABLE(
  -- Timestamp of start of a second.
  ts TIMESTAMP,
  -- Sum of average utilization over period.
  -- Note: as the data is normalized, the values will be in the
  -- [0, 1] range.
  utilization DOUBLE,
  -- Sum of average utilization over all CPUs over period.
  -- Note: as the data is unnormalized, the values will be in the
  -- [0, cpu_count] range.
  unnormalized_utilization DOUBLE
)
AS
SELECT * FROM cpu_thread_utilization_per_period(time_from_s(1), $utid);

-- Aggregated CPU statistics for each thread.
CREATE PERFETTO PIPELINE cpu_cycles_per_thread(
  -- Thread
  utid JOINID(thread.id),
  -- Sum of CPU millicycles
  millicycles LONG,
  -- Sum of CPU megacycles
  megacycles LONG,
  -- Total runtime duration
  runtime DURATION,
  -- Minimum CPU frequency in kHz
  min_freq LONG,
  -- Maximum CPU frequency in kHz
  max_freq LONG,
  -- Average CPU frequency in kHz
  avg_freq LONG
)
MATERIALIZED AS
FROM cpu_cycles_per_thread_per_cpu
|> AGGREGATE
     sum(millicycles) AS millicycles,
     cast_int!(SUM(millicycles) / 1e9) AS megacycles,
     sum(runtime) AS runtime,
     min(min_freq) AS min_freq,
     max(max_freq) AS max_freq,
     cast_int!(SUM(millicycles) / (SUM(runtime) / 1000)) AS avg_freq
   GROUP BY utid
|> SELECT utid, millicycles, megacycles, runtime, min_freq, max_freq, avg_freq;

-- Aggregated CPU statistics for each thread in a provided interval.
--
-- This function is only designed to run over a small number of intervals
-- (10-100 at most). It will be *very slow* for large sets of intervals.
CREATE PERFETTO MACRO cpu_cycles_per_thread_in_interval(
  -- Start of the interval.
  ts Expr,
  -- Duration of the interval.
  dur Expr
)
-- Returns: (utid JOINID(thread.id), millicycles LONG, megacycles LONG,
-- runtime DURATION, awake_runtime DURATION, min_freq LONG, max_freq LONG,
-- avg_freq LONG).
RETURNS Pipeline AS (
  _interval_intersect_single!($ts, $dur, _cpu_freq_per_thread)
  |> AGGREGATE
       cast_int!(SUM(dur * freq / 1000)) AS millicycles,
       cast_int!(SUM(dur * freq / 1000) / 1e9) AS megacycles,
       sum(dur) AS runtime,
       sum(to_monotonic(ts + dur) - to_monotonic(ts)) AS awake_runtime,
       min(freq) AS min_freq,
       max(freq) AS max_freq,
       cast_int!(SUM((dur * freq / 1000))
         / (SUM(CASE WHEN freq IS NOT NULL THEN dur END) / 1000)) AS avg_freq
     GROUP BY utid
);

-- Returns a table of thread utilization over a given interval.
--
-- Utilization is computed as runtime over the duration of the interval, aggregated by UTID.
-- Utilization can be normalized (divide by number of CPUs) or unnormalized.
--
-- This function is only designed to run over a small number of intervals
-- (10-100 at most). It will be *very slow* for large sets of intervals.
CREATE PERFETTO MACRO cpu_thread_utilization_in_interval(
  -- Start of the interval.
  ts Expr,
  -- Duration of the interval.
  dur Expr
)
-- Returns: (upid JOINID(process.id), utid JOINID(thread.id),
-- thread_name STRING, awake_dur LONG, awake_utilization DOUBLE,
-- awake_unnormalized_utilization DOUBLE).
RETURNS Pipeline AS (
  cpu_cycles_per_thread_in_interval!($ts, $dur)
  |> JOIN thread USING (utid)
  |> AGGREGATE
       ANY_VALUE(upid) AS upid,
       ANY_VALUE(thread.name) AS thread_name,
       sum(awake_runtime) AS awake_dur,
       round(
         sum(awake_runtime) * 100.0 / (to_monotonic($ts + $dur) - to_monotonic($ts))
         / (SELECT max(cpu) + 1 FROM cpu),
         6
       ) AS awake_utilization,
       round(
         sum(awake_runtime) * 100.0 / (to_monotonic($ts + $dur) - to_monotonic($ts)),
         6
       ) AS awake_unnormalized_utilization
     GROUP BY utid
);
