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

DROP TABLE IF EXISTS android_thread_time_in_state_base;
CREATE TABLE android_thread_time_in_state_base AS
SELECT
  base.*,
  IFNULL(core_type_per_cpu.core_type, 'unknown') core_type
FROM (
  SELECT
    ts,
    utid,
    EXTRACT_ARG(counter.arg_set_id, 'time_in_state_cpu_id') AS
        time_in_state_cpu,
    EXTRACT_ARG(counter.arg_set_id, 'freq') AS freq,
    CAST(value AS INT) AS runtime_ms_counter
  FROM counter
  JOIN thread_counter_track ON (counter.track_id = thread_counter_track.id)
  WHERE thread_counter_track.name = 'time_in_state'
) base
LEFT JOIN core_type_per_cpu ON (cpu = time_in_state_cpu);

DROP VIEW IF EXISTS android_thread_time_in_state_raw;
CREATE VIEW android_thread_time_in_state_raw AS
SELECT
  utid,
  time_in_state_cpu,
  core_type,
  freq,
  MAX(runtime_ms_counter) - MIN(runtime_ms_counter) runtime_ms_diff
FROM android_thread_time_in_state_base
GROUP BY utid, time_in_state_cpu, core_type, freq;

DROP TABLE IF EXISTS android_thread_time_in_state_counters;
CREATE TABLE android_thread_time_in_state_counters AS
SELECT
  utid,
  raw.time_in_state_cpu,
  raw.core_type,
  SUM(runtime_ms_diff) AS runtime_ms,
  SUM(raw.freq * runtime_ms_diff / 1000000) AS mcycles,
  SUM(power * runtime_ms_diff / 3600000) AS power_profile_mah
FROM android_thread_time_in_state_raw AS raw
    LEFT OUTER JOIN cpu_cluster_power AS power USING(core_type, freq)
GROUP BY utid, raw.time_in_state_cpu, raw.core_type
HAVING runtime_ms > 0;

DROP VIEW IF EXISTS android_thread_time_in_state_thread_metrics;
CREATE VIEW android_thread_time_in_state_thread_metrics AS
SELECT
  utid,
  RepeatedField(AndroidThreadTimeInStateMetric_MetricsByCoreType(
    'time_in_state_cpu',  time_in_state_cpu,
    'core_type', core_type,
    'runtime_ms', runtime_ms,
    'mcycles', CAST(mcycles AS INT),
    'power_profile_mah', power_profile_mah
  )) metrics
FROM android_thread_time_in_state_counters
GROUP BY utid;

DROP VIEW IF EXISTS android_thread_time_in_state_threads;
CREATE VIEW android_thread_time_in_state_threads AS
SELECT
  upid,
  RepeatedField(AndroidThreadTimeInStateMetric_Thread(
    'name',
    thread.name,
    'main_thread',
    thread.is_main_thread,
    'metrics_by_core_type',
    android_thread_time_in_state_thread_metrics.metrics
  )) threads
FROM thread
JOIN android_thread_time_in_state_thread_metrics USING (utid)
GROUP BY upid;

DROP VIEW IF EXISTS android_thread_time_in_state_process_metrics;
CREATE VIEW android_thread_time_in_state_process_metrics AS
WITH process_counters AS (
  SELECT
    upid,
    time_in_state_cpu,
    core_type,
    SUM(runtime_ms) AS runtime_ms,
    SUM(mcycles) AS mcycles,
    SUM(power_profile_mah) AS power_profile_mah
  FROM android_thread_time_in_state_counters
  JOIN thread USING (utid)
  GROUP BY upid, time_in_state_cpu, core_type
)
SELECT
  upid,
  RepeatedField(AndroidThreadTimeInStateMetric_MetricsByCoreType(
    'time_in_state_cpu', time_in_state_cpu,
    'core_type', core_type,
    'runtime_ms', runtime_ms,
    'mcycles', CAST(mcycles AS INT),
    'power_profile_mah', power_profile_mah
 )) metrics
FROM process_counters
GROUP BY upid;

DROP VIEW IF EXISTS android_thread_time_in_state_output;
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

DROP VIEW IF EXISTS android_thread_time_in_state_event_raw;
CREATE VIEW android_thread_time_in_state_event_raw AS
SELECT
  ts,
  utid,
  core_type,
  freq,
  time_in_state_cpu,
  runtime_ms_counter - LAG(runtime_ms_counter) OVER win AS runtime_ms_diff
FROM android_thread_time_in_state_base
WINDOW win AS (PARTITION BY utid, core_type, time_in_state_cpu, freq ORDER BY ts);

DROP VIEW IF EXISTS android_thread_time_in_state_by_core_type;
CREATE VIEW android_thread_time_in_state_by_core_type AS
SELECT
  ts,
  utid,
  core_type,
  freq,
  SUM(runtime_ms_diff) runtime_ms_diff
FROM android_thread_time_in_state_event_raw
GROUP BY ts, utid, core_type, freq
HAVING runtime_ms_diff > 0;

DROP VIEW IF EXISTS android_thread_time_in_state_event_thread;
CREATE VIEW android_thread_time_in_state_event_thread AS
SELECT
   -- We need globally unique track names so add the utid even when we
  -- know the name. But when we don't, also use the tid because that's what
  -- the rest of the UI does.
  IFNULL(thread.name, 'Thread ' || thread.tid) || ' (' || thread.utid || ')' 
    || ' (' || core_type || ' core) mcycles' AS track_name,
  ts,
  upid,
  SUM(runtime_ms_diff * freq) OVER win AS cycles
FROM android_thread_time_in_state_by_core_type
JOIN thread USING (utid)
GROUP BY ts, upid, track_name
WINDOW win AS (PARTITION BY utid ORDER BY ts);

DROP VIEW IF EXISTS android_thread_time_in_state_event_global;
CREATE VIEW android_thread_time_in_state_event_global AS
SELECT
  'Total ' || core_type || ' core mcycles' as track_name,
  ts,
  0 AS upid,
  SUM(runtime_ms_diff * freq) OVER win as cycles
FROM android_thread_time_in_state_by_core_type
GROUP BY ts, track_name
WINDOW win AS (ORDER BY ts);

DROP TABLE IF EXISTS android_thread_time_in_state_event;
CREATE TABLE android_thread_time_in_state_event AS
SELECT 'counter' as track_type, track_name, upid, ts, cycles / 1000000.0 AS value
FROM android_thread_time_in_state_event_thread
UNION ALL
SELECT 'counter' as track_type, track_name, upid, ts, cycles / 1000000.0 AS value
FROM android_thread_time_in_state_event_global
-- Biggest values at top of list in UI.
ORDER BY value DESC;
