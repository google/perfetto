--
-- Copyright 2020 The Android Open Source Project
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
--

SELECT RUN_METRIC('android/cpu_info.sql');
SELECT RUN_METRIC('android/process_metadata.sql');

CREATE VIEW android_thread_time_in_state_tracks AS
SELECT
  id AS track_id,
  utid,
  cast(REPLACE(REPLACE(name, '.time_in_state', ''), 'cpu', '') AS int) AS cpu
FROM thread_counter_track
WHERE name LIKE "cpu%.time_in_state";

CREATE TABLE android_thread_time_in_state_counters AS
SELECT
  utid,
  (
    SELECT
      CASE
        WHEN layout = 'big_little_bigger' AND cpu < 4 THEN 'little'
        WHEN layout = 'big_little_bigger' AND cpu < 7 THEN 'big'
        WHEN layout = 'big_little_bigger' AND cpu = 7 THEN 'bigger'
        WHEN layout = 'big_little' AND cpu < 4 THEN 'little'
        WHEN layout = 'big_little' AND cpu < 8 THEN 'big'
        ELSE 'unknown'
      END
    FROM core_layout_type
  ) AS core_type,
  CAST((MAX(counter.value) - MIN(counter.value)) AS int) runtime_ms
FROM counter
JOIN android_thread_time_in_state_tracks USING (track_id)
GROUP BY 1, 2
HAVING runtime_ms > 0;

CREATE VIEW android_thread_time_in_state_thread_metrics AS
SELECT
  utid,
  RepeatedField(AndroidThreadTimeInStateMetric_MetricsByCoreType(
    'core_type', core_type,
    'runtime_ms', runtime_ms
  )) metrics
FROM android_thread_time_in_state_counters
GROUP BY 1;

CREATE VIEW android_thread_time_in_state_threads AS
SELECT
  upid,
  RepeatedField(AndroidThreadTimeInStateMetric_Thread(
    'name', thread.name,
    'metrics_by_core_type', android_thread_time_in_state_thread_metrics.metrics
  )) threads
FROM thread
JOIN android_thread_time_in_state_thread_metrics USING (utid)
GROUP BY 1;

CREATE VIEW android_thread_time_in_state_process_metrics AS
WITH process_counters AS (
  SELECT
    upid,
    core_type,
    SUM(runtime_ms) runtime_ms
  FROM android_thread_time_in_state_counters
  JOIN thread USING (utid)
  GROUP BY 1, 2
)
SELECT
  upid,
  RepeatedField(AndroidThreadTimeInStateMetric_MetricsByCoreType(
    'core_type', core_type,
    'runtime_ms', runtime_ms
  )) metrics
FROM process_counters
GROUP BY 1;

CREATE VIEW android_thread_time_in_state_output AS
SELECT AndroidThreadTimeInStateMetric(
  'processes', (
    SELECT
      RepeatedField(AndroidThreadTimeInStateMetric_Process(
        'metadata', metadata,
        'metrics_by_core_type',
            android_thread_time_in_state_process_metrics.metrics,
        'threads', android_thread_time_in_state_threads.threads
      ))
    FROM process
    JOIN process_metadata USING (upid)
    JOIN android_thread_time_in_state_process_metrics USING (upid)
    JOIN android_thread_time_in_state_threads USING (upid)
  )
);
