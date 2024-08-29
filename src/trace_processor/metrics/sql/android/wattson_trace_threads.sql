
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

INCLUDE PERFETTO MODULE wattson.curves.grouped;
INCLUDE PERFETTO MODULE viz.summary.threads_w_processes;

DROP VIEW IF EXISTS _wattson_period_windows;
CREATE PERFETTO VIEW _wattson_period_windows AS
SELECT
  MIN(ts) as ts,
  MAX(ts) - MIN(ts) as dur,
  1 as period_id
FROM _system_state_mw;

SELECT RUN_METRIC(
  'android/wattson_tasks_attribution.sql',
  'window_table',
  '_wattson_period_windows'
);

-- Group by unique thread ID and disregard CPUs, summing of power over all CPUs
-- and all instances of the thread
DROP VIEW IF EXISTS _wattson_thread_attribution;
CREATE PERFETTO VIEW _wattson_thread_attribution AS
SELECT
  -- active time of thread divided by total time of trace
  SUM(estimated_mw * dur) / 1000000000 as estimated_mws,
  (
    SUM(estimated_mw * dur) / (SELECT SUM(dur) from _windowed_wattson)
  ) as estimated_mw,
  thread_name,
  process_name,
  tid,
  pid
FROM _windowed_threads_system_state
GROUP BY utid
ORDER BY estimated_mw DESC;

DROP VIEW IF EXISTS wattson_trace_threads_output;
CREATE PERFETTO VIEW wattson_trace_threads_output AS
SELECT AndroidWattsonTasksAttributionMetric(
  'metric_version', 2,
  'task_info', (
    SELECT RepeatedField(
      AndroidWattsonTaskInfo(
        'estimated_mws', ROUND(estimated_mws, 6),
        'estimated_mw', ROUND(estimated_mw, 6),
        'thread_name', thread_name,
        'process_name', process_name,
        'thread_id', tid,
        'process_id', pid
      )
    )
    FROM _wattson_thread_attribution
  )
);
