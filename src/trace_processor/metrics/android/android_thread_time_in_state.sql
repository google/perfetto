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

CREATE VIEW android_thread_time_in_state_base AS
SELECT
  *,
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
  ) AS core_type
FROM (
  SELECT
    slice.ts AS ts,
    thread.upid AS upid,
    CAST(SUBSTR(slice.name, 18) AS int) AS cpu,
    thread.utid AS utid,
    -- We need globally unique track names so add the utid even when we
    -- know the name. But when we don't, also use the tid because that's what
    -- the rest of the UI does.
    IFNULL(thread.name, 'Thread ' || thread.tid) || ' (' || thread.utid || ')'
        AS thread_name,
    args.key AS freq,
    args.int_value as runtime_ms_counter
  FROM slice
    JOIN thread_track ON slice.track_id = thread_track.id
    JOIN args USING (arg_set_id)
    JOIN thread USING (utid)
  WHERE slice.name LIKE 'time_in_state.%'
);

CREATE VIEW android_thread_time_in_state_raw AS
SELECT
  utid,
  core_type,
  freq,
  MAX(runtime_ms_counter) - MIN(runtime_ms_counter) runtime_ms
FROM android_thread_time_in_state_base
GROUP BY utid, core_type, freq;

CREATE TABLE android_thread_time_in_state_counters AS
SELECT
  utid,
  core_type,
  SUM(runtime_ms) runtime_ms
FROM android_thread_time_in_state_raw
GROUP BY utid, core_type
HAVING runtime_ms > 0;

CREATE VIEW android_thread_time_in_state_thread_metrics AS
SELECT
  utid,
  RepeatedField(AndroidThreadTimeInStateMetric_MetricsByCoreType(
    'core_type', core_type,
    'runtime_ms', runtime_ms
  )) metrics
FROM android_thread_time_in_state_counters
GROUP BY utid;

CREATE VIEW android_thread_time_in_state_threads AS
SELECT
  upid,
  RepeatedField(AndroidThreadTimeInStateMetric_Thread(
    'name', thread.name,
    'metrics_by_core_type', android_thread_time_in_state_thread_metrics.metrics
  )) threads
FROM thread
JOIN android_thread_time_in_state_thread_metrics USING (utid)
GROUP BY upid;

CREATE VIEW android_thread_time_in_state_process_metrics AS
WITH process_counters AS (
  SELECT
    upid,
    core_type,
    SUM(runtime_ms) runtime_ms
  FROM android_thread_time_in_state_counters
  JOIN thread USING (utid)
  GROUP BY upid, core_type
)
SELECT
  upid,
  RepeatedField(AndroidThreadTimeInStateMetric_MetricsByCoreType(
    'core_type', core_type,
    'runtime_ms', runtime_ms
  )) metrics
FROM process_counters
GROUP BY upid;

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

CREATE VIEW android_thread_time_in_state_annotations_raw AS
SELECT
  ts,
  ts - LAG(ts) OVER (PARTITION BY core_type, utid ORDER BY ts) AS dur,
  upid,
  core_type,
  utid,
  thread_name,
  freq,
  runtime_ms_counter - LAG(runtime_ms_counter)
      OVER (PARTITION BY core_type, utid, freq ORDER BY ts) AS runtime_ms
FROM android_thread_time_in_state_base;

CREATE VIEW android_thread_time_in_state_annotations AS
SELECT
  'counter' AS track_type,
  thread_name || ' (' || core_type || ' core)' as track_name,
  ts,
  dur,
  upid,
  sum(runtime_ms * freq / 1000) AS value -- i.e. cycles
FROM android_thread_time_in_state_annotations_raw
WHERE thread_name IS NOT NULL
  AND runtime_ms IS NOT NULL
  AND dur != 0
GROUP BY track_type, track_name, ts, dur, upid
-- Biggest values at top of list in UI.
ORDER BY value DESC;
