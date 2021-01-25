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


DROP TABLE IF EXISTS android_jank_process_allowlist;
CREATE TABLE android_jank_process_allowlist AS
SELECT process.name, process.upid
FROM process
WHERE process.name IN (
  'com.android.systemui',
  'com.google.android.apps.nexuslauncher',
  'com.google.android.inputmethod.latin'
);

SELECT RUN_METRIC(
  'android/android_hwui_threads.sql',
  'table_name_prefix', 'android_jank',
  'process_allowlist_table', 'android_jank_process_allowlist');

DROP TABLE IF EXISTS android_jank_thread_state_running;
CREATE TABLE android_jank_thread_state_running AS
SELECT utid, ts, dur, state
FROM thread_state
WHERE utid IN (SELECT utid FROM android_jank_main_thread_slices)
AND state = 'Running'
AND dur > 0;

DROP TABLE IF EXISTS android_jank_thread_state_scheduled;
CREATE TABLE android_jank_thread_state_scheduled AS
SELECT utid, ts, dur, state
FROM thread_state
WHERE utid IN (SELECT utid FROM android_jank_main_thread_slices)
AND (state = 'R' OR state = 'R+')
AND dur > 0;

DROP TABLE IF EXISTS android_jank_thread_state_io_wait;
CREATE TABLE android_jank_thread_state_io_wait AS
SELECT utid, ts, dur, state
FROM thread_state
WHERE utid IN (SELECT utid FROM android_jank_main_thread_slices)
AND (((state = 'D' OR state = 'DK') AND io_wait) OR (state = 'DK' AND io_wait IS NULL))
AND dur > 0;

--
-- Main Thread alerts
--

-- Expensive measure/layout

DROP TABLE IF EXISTS android_jank_measure_layout_slices;
CREATE TABLE android_jank_measure_layout_slices AS
SELECT
  process_name,
  utid,
  id,
  ts,
  dur
FROM android_jank_main_thread_slices
WHERE name in ('measure', 'layout')
AND dur >= 3000000;

CREATE VIRTUAL TABLE IF NOT EXISTS android_jank_measure_layout_slices_state
USING span_join(android_jank_measure_layout_slices PARTITIONED utid, android_jank_thread_state_running PARTITIONED utid);

DROP TABLE IF EXISTS android_jank_measure_layout_slices_high_cpu;
CREATE TABLE android_jank_measure_layout_slices_high_cpu AS
SELECT id FROM android_jank_measure_layout_slices_state
GROUP BY id
HAVING SUM(dur) > 3000000;

DROP TABLE IF EXISTS android_jank_measure_layout_alerts;
CREATE TABLE android_jank_measure_layout_alerts AS
SELECT
  process_name,
  ts,
  dur,
  'Expensive measure/layout pass' as alert_name,
  id
FROM android_jank_measure_layout_slices
JOIN android_jank_measure_layout_slices_high_cpu USING (id);

-- Inflation during ListView recycling
-- as additional alerts for expensive layout slices

DROP TABLE IF EXISTS android_jank_listview_inflation_alerts;
CREATE TABLE android_jank_listview_inflation_alerts AS
SELECT
  process_name,
  ts,
  dur,
  'Inflation during ListView recycling' as alert_name
FROM android_jank_main_thread_slices
WHERE name IN ('obtainView', 'setupListItem')
AND EXISTS (
  SELECT 1
  FROM descendant_slice(android_jank_main_thread_slices.id)
  WHERE name = 'inflate')
AND EXISTS(
  SELECT 1
  FROM android_jank_measure_layout_alerts
  JOIN ancestor_slice(android_jank_main_thread_slices.id) USING (id)
);

-- Long View#draw()

DROP TABLE IF EXISTS android_jank_view_draw_slices;
CREATE TABLE android_jank_view_draw_slices AS
SELECT
  process_name,
  utid,
  id,
  ts,
  dur
FROM android_jank_main_thread_slices
WHERE name in ('getDisplayList', 'Record View#draw()')
AND dur >= 3000000;

CREATE VIRTUAL TABLE IF NOT EXISTS android_jank_view_draw_slices_state
USING span_join(android_jank_view_draw_slices PARTITIONED utid, android_jank_thread_state_running PARTITIONED utid);

DROP TABLE IF EXISTS android_jank_view_draw_slices_high_cpu;
CREATE TABLE android_jank_view_draw_slices_high_cpu AS
SELECT id FROM android_jank_view_draw_slices_state
GROUP BY id
HAVING SUM(dur) > 3000000;

DROP TABLE IF EXISTS android_jank_view_draw_alerts;
CREATE TABLE android_jank_view_draw_alerts AS
SELECT
  process_name,
  ts,
  dur,
  'Long View#draw()' as alert_name
FROM android_jank_main_thread_slices
JOIN android_jank_view_draw_slices_high_cpu USING (id);

-- Scheduling delay and Blocking I/O delay

DROP TABLE IF EXISTS android_jank_do_frame_slices;
CREATE TABLE android_jank_do_frame_slices AS
SELECT
  process_name,
  utid,
  id,
  ts,
  dur
FROM android_jank_main_thread_slices
WHERE name = 'Choreographer#doFrame'
AND dur >= 5000000;

CREATE VIRTUAL TABLE IF NOT EXISTS android_jank_do_frame_slices_state_scheduled
USING span_join(android_jank_do_frame_slices PARTITIONED utid, android_jank_thread_state_scheduled PARTITIONED utid);


DROP TABLE IF EXISTS android_jank_do_frame_slices_long_scheduled;
CREATE TABLE android_jank_do_frame_slices_long_scheduled AS
SELECT id FROM android_jank_do_frame_slices_state_scheduled
GROUP BY id
HAVING SUM(dur) > 5000000;

DROP TABLE IF EXISTS android_jank_scheduling_delay_alerts;
CREATE TABLE android_jank_scheduling_delay_alerts AS
SELECT
  process_name,
  ts,
  dur,
  'Scheduling delay' as alert_name
FROM android_jank_do_frame_slices
JOIN android_jank_do_frame_slices_long_scheduled USING (id);

CREATE VIRTUAL TABLE IF NOT EXISTS android_jank_do_frame_slices_state_io_wait
USING span_join(android_jank_do_frame_slices PARTITIONED utid, android_jank_thread_state_io_wait PARTITIONED utid);

DROP TABLE IF EXISTS android_jank_do_frame_slices_long_io_wait;
CREATE TABLE android_jank_do_frame_slices_long_io_wait AS
SELECT id FROM android_jank_do_frame_slices_state_io_wait
GROUP BY id
HAVING SUM(dur) > 5000000;

DROP TABLE IF EXISTS android_jank_blocking_delay_alerts;
CREATE TABLE android_jank_blocking_delay_alerts AS
SELECT
  process_name,
  ts,
  dur,
  'Blocking I/O delay' as alert_name
FROM android_jank_do_frame_slices
JOIN android_jank_do_frame_slices_long_io_wait USING (id);

--
-- Render Thread alerts
--

-- Expensive Canvas#saveLayer()

DROP TABLE IF EXISTS android_jank_save_layer_alerts;
CREATE TABLE android_jank_save_layer_alerts AS
SELECT
  process_name,
  ts,
  dur,
  'Expensive rendering with Canvas#saveLayer()' as alert_name
FROM android_jank_render_thread_slices
WHERE name LIKE '%alpha caused %saveLayer %'
AND dur >= 1000000;

-- Path texture churn

DROP TABLE IF EXISTS android_jank_generate_path_alerts;
CREATE TABLE android_jank_generate_path_alerts AS
SELECT
  process_name,
  ts,
  dur,
  'Path texture churn' as alert_name
FROM android_jank_render_thread_slices
WHERE name = 'Generate Path Texture'
AND dur >= 3000000;

-- Expensive Bitmap uploads

DROP TABLE IF EXISTS android_jank_upload_texture_alerts;
CREATE TABLE android_jank_upload_texture_alerts AS
SELECT
  process_name,
  ts,
  dur,
  'Expensive Bitmap uploads' as alert_name
FROM android_jank_render_thread_slices
WHERE name LIKE 'Upload %x% Texture'
AND dur >= 3000000;

-- Merge all alerts tables into one table
DROP TABLE IF EXISTS android_jank_alerts;
CREATE TABLE android_jank_alerts AS
SELECT process_name, ts, dur, alert_name FROM android_jank_measure_layout_alerts
UNION ALL
SELECT process_name, ts, dur, alert_name FROM android_jank_listview_inflation_alerts
UNION ALL
SELECT process_name, ts, dur, alert_name FROM android_jank_scheduling_delay_alerts
UNION ALL
SELECT process_name, ts, dur, alert_name FROM android_jank_blocking_delay_alerts
UNION ALL
SELECT process_name, ts, dur, alert_name FROM android_jank_save_layer_alerts
UNION ALL
SELECT process_name, ts, dur, alert_name FROM android_jank_generate_path_alerts
UNION ALL
SELECT process_name, ts, dur, alert_name FROM android_jank_upload_texture_alerts;

DROP VIEW IF EXISTS android_jank_event;
CREATE VIEW android_jank_event AS
SELECT
  'slice' as track_type,
  process_name || ' warnings' as track_name,
  ts,
  dur,
  alert_name as slice_name
FROM android_jank_alerts;

DROP VIEW IF EXISTS android_jank_output;
CREATE VIEW android_jank_output AS
SELECT AndroidJankMetrics(
  'warnings', (
    SELECT RepeatedField(
      AndroidJankMetrics_Warning(
       'ts', ts,
       'dur', dur,
       'process_name', process_name,
       'warning_text', alert_name))
    FROM android_jank_alerts
    ORDER BY process_name, ts, dur));
