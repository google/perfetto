--
-- Copyright 2025 The Android Open Source Project
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

INCLUDE PERFETTO MODULE linux.cpu.utilization.general;

INCLUDE PERFETTO MODULE intervals.intersect;

-- NOTE (psqlnext): the `*_in_interval` construct is parameterized by a scalar
-- window, so it is a pipeline-valued macro (`RETURNS Pipeline`) clipping
-- `_cpu_freq_per_thread` with `_interval_intersect_single!($ts, $dur, …)`.

-- Aggregated CPU statistics for each thread per CPU combination.
-- To operate properly this requires sched/sched_switch and power/cpu_frequency
-- ftrace events to be present in the trace.
CREATE PERFETTO PIPELINE cpu_cycles_per_thread_per_cpu(
  -- Thread
  utid JOINID(thread.id),
  -- Unique CPU id. Joinable with `cpu.id`.
  ucpu JOINID(cpu.id),
  -- The number of the CPU. Might not be the same as ucpu in multi machine cases.
  cpu LONG,
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
) MATERIALIZED AS
FROM _cpu_freq_per_thread
|> JOIN cpu USING (ucpu)
|> AGGREGATE
  cast_int!(SUM(dur * freq / 1000)) AS millicycles,
  cast_int!(SUM(dur * freq / 1000) / 1e9) AS megacycles,
  sum(dur) AS runtime,
  min(freq) AS min_freq,
  max(freq) AS max_freq,
  cast_int!(SUM((dur * freq / 1000)) / (SUM(dur) / 1000)) AS avg_freq
  GROUP BY utid, ucpu, cpu.cpu
|> SELECT
  utid,
  ucpu,
  cpu.cpu AS cpu,
  millicycles,
  megacycles,
  runtime,
  min_freq,
  max_freq,
  avg_freq;

-- Aggregated CPU statistics for each thread per CPU combination in a provided interval.
-- To operate properly this requires sched/sched_switch and power/cpu_frequency
-- ftrace events to be present in the trace.
-- Warning: this query is expensive and might take a long time to execute when joined
-- with a large number of intervals.
CREATE PERFETTO MACRO cpu_cycles_per_thread_per_cpu_in_interval(
  -- Start of the interval.
  ts Expr,
  -- Duration of the interval.
  dur Expr
)
-- Returns: (utid JOINID(thread.id), ucpu JOINID(cpu.id), cpu LONG,
-- millicycles LONG, megacycles LONG, runtime DURATION, min_freq LONG,
-- max_freq LONG, avg_freq LONG).
RETURNS Pipeline AS (
  _interval_intersect_single!($ts, $dur, _cpu_freq_per_thread)
  |> JOIN cpu USING (ucpu)
  |> AGGREGATE
       cast_int!(SUM(dur * freq / 1000)) AS millicycles,
       cast_int!(SUM(dur * freq / 1000) / 1e9) AS megacycles,
       sum(dur) AS runtime,
       min(freq) AS min_freq,
       max(freq) AS max_freq,
       cast_int!(SUM((dur * freq / 1000))
         / (SUM(CASE WHEN freq IS NOT NULL THEN dur END) / 1000)) AS avg_freq
     GROUP BY utid, ucpu, cpu.cpu
  |> SELECT
       utid,
       ucpu,
       cpu.cpu AS cpu,
       millicycles,
       megacycles,
       runtime,
       min_freq,
       max_freq,
       avg_freq
);
