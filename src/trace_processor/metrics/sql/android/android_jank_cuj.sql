--
-- Copyright 2022 The Android Open Source Project
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

SELECT RUN_METRIC('android/process_metadata.sql');
INCLUDE PERFETTO MODULE android.surfaceflinger;
INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;
INCLUDE PERFETTO MODULE android.cujs.sysui_cuj_counters;
INCLUDE PERFETTO MODULE android.frames.jank_type;
INCLUDE PERFETTO MODULE android.frames.timeline;

-- Table captures additional data related to each frame in a CUJ. This information includes
-- data like missed type of jank, missed app/sf frame, missed sf/hwui callbacks etc.
DROP TABLE IF EXISTS _android_jank_cuj_frames_data;
CREATE PERFETTO TABLE _android_jank_cuj_frames_data AS
WITH actual_timeline_with_vsync AS (
  SELECT
    *,
    CAST(name AS INTEGER) AS vsync
  FROM actual_frame_timeline_slice
  WHERE dur > 0
),
expected_timeline_with_vsync AS (
  SELECT *, CAST(name AS INTEGER) AS vsync
  FROM expected_frame_timeline_slice
  WHERE dur > 0
),
frames_in_cuj AS (
  SELECT
    frame.upid,
    frame.layer_name AS frame_layer_name,
    frame.frame_id,
    actual_frame.display_frame_token,
    cuj_id,
    frame_ts,
    (
      frame_ts + frame.dur
    ) AS ts_end,
    jank_type,
    on_time_finish,
    sf_callback_missed,
    hwui_callback_missed
  FROM _android_frames_in_cuj frame
  JOIN actual_timeline_with_vsync AS actual_frame
    ON frame.frame_id = actual_frame.vsync
  LEFT JOIN _vsync_missed_callback AS missed_callback USING(vsync)
  WHERE
    frame.cuj_layer_id IS NULL
    OR (
      actual_frame.layer_name GLOB '*#*'
      AND frame.cuj_layer_id
        = android_get_layer_id_from_name(actual_frame.layer_name)
      AND frame.layer_id
        = android_get_layer_id_from_name(actual_frame.layer_name))
)
SELECT
  ROW_NUMBER() OVER (PARTITION BY cuj_id ORDER BY frame_id ASC) AS frame_number,
  cuj_id,
  -- We use MAX to check if at least one of the layers jank_type matches the pattern
  MAX(android_is_app_jank_type(jank_type)) AS app_missed,
  -- We use MAX to check if at least one of the layers jank_type matches the pattern
  MAX(android_is_sf_jank_type(jank_type)) AS sf_missed,
  IFNULL(MAX(sf_callback_missed), 0) AS sf_callback_missed,
  IFNULL(MAX(hwui_callback_missed), 0) AS hwui_callback_missed,
  -- We use MIN to check if ALL layers finished on time
  MIN(on_time_finish) AS on_time_finish,
  frame_id,
  frame_layer_name,
  vsync_boundary.display_frame_token,
  e.ts AS ts_expected,
  -- In cases where we are drawing multiple layers, there will be  one
  -- expected frame timeline slice, but multiple actual frame timeline slices.
  -- As a simplification we just take here the min(ts) and max(ts_end) of
  -- the actual frame timeline slices.
  MIN(frame_ts) AS ts_actual_min,
  MAX(ts_end) AS ts_end_actual_max,
  -- In case expected timeline is missing, as a fallback we use the typical frame deadline
  -- for 60Hz.
  COALESCE(MAX(e.dur), 16600000) AS dur_expected
FROM frames_in_cuj vsync_boundary
JOIN expected_timeline_with_vsync e
  ON e.upid = vsync_boundary.upid  AND e.vsync = vsync_boundary.frame_id
GROUP BY cuj_id, e.vsync, e.ts;

-- Combine trace frame data with Choreographer#doFrame
DROP TABLE IF EXISTS android_jank_cuj_frame_trace_data;
CREATE PERFETTO TABLE android_jank_cuj_frame_trace_data AS
WITH do_frame_ordered AS (
  SELECT
    *,
    -- ts_end of the previous do_frame, or -1 if no previous do_frame found
    COALESCE(LAG(ts_end) OVER (PARTITION BY cuj_id ORDER BY vsync ASC), -1) AS ts_prev_do_frame_end
  FROM _android_jank_cuj_do_frames
),
trace_metrics_frame AS (
  select timeline.*,
   do_frame.ts AS ts_do_frame_start,
   CASE
       WHEN ts_expected IS NULL
         THEN do_frame.ts
       ELSE MAX(do_frame.ts_prev_do_frame_end, timeline.ts_expected)
   END AS ts
   FROM do_frame_ordered do_frame
   LEFT JOIN _android_jank_cuj_frames_data timeline
    ON do_frame.cuj_id = timeline.cuj_id AND do_frame.vsync = timeline.frame_id
)
SELECT
  cuj_id,
  frame_number,
  frame_id,
  ts,
  ts_expected,
  ts_do_frame_start,
  app_missed,
  sf_missed,
  sf_callback_missed,
  hwui_callback_missed,
  on_time_finish,
  ts_end_actual_max - ts AS dur,
  ts_end_actual_max - ts_do_frame_start AS dur_unadjusted,
  dur_expected,
  ts_end_actual_max AS ts_end,
  frame_layer_name
FROM trace_metrics_frame;

-- Table captures CUJ scoped frame data for the SF process.
DROP TABLE IF EXISTS android_jank_cuj_sf_frame_trace_data;
CREATE PERFETTO TABLE android_jank_cuj_sf_frame_trace_data AS
WITH android_jank_cuj_sf_frame_base AS (
  SELECT DISTINCT
    boundary.cuj_id,
    boundary.vsync,
    boundary.ts,
    boundary.ts_main_thread_start,
    boundary.ts_end,
    boundary.dur,
    actual_timeline.jank_tag = 'Self Jank' AS sf_missed,
    NULL AS app_missed, -- for simplicity align schema with android_jank_cuj_frame
    jank_tag,
    jank_type,
    prediction_type,
    present_type,
    gpu_composition,
    -- In case expected timeline is missing, as a fallback we use the typical frame deadline
    -- for 60Hz.
    COALESCE(expected_timeline.dur, 16600000) AS dur_expected
  FROM _android_jank_cuj_sf_main_thread_frame_boundary boundary
  JOIN _android_sf_process sf_process
  JOIN actual_frame_timeline_slice actual_timeline
    ON actual_timeline.upid = sf_process.upid
      AND boundary.vsync = CAST(actual_timeline.name AS INTEGER)
  JOIN _android_jank_cuj_frames_data ft
    ON CAST(actual_timeline.name AS INTEGER) = ft.display_frame_token
      AND boundary.cuj_id = ft.cuj_id
  LEFT JOIN expected_frame_timeline_slice expected_timeline
    ON expected_timeline.upid = actual_timeline.upid
      AND expected_timeline.name = actual_timeline.name
)
SELECT
 *,
 ROW_NUMBER() OVER (PARTITION BY cuj_id ORDER BY vsync ASC) AS frame_number
FROM android_jank_cuj_sf_frame_base;

-- Table captures various missed frames and callbacks counters from counter tracks in a process.
DROP TABLE IF EXISTS android_jank_cuj_counter_metrics;
CREATE PERFETTO TABLE android_jank_cuj_counter_metrics AS
-- Order CUJs to get the ts of the next CUJ with the same name.
-- This is to avoid selecting counters logged for the next CUJ in case multiple
-- CUJs happened in a short succession.
WITH cujs_ordered AS (
  SELECT
    cuj_id,
    cuj_name,
    cuj_slice_name,
    upid,
    state,
    ts_end,
    CASE
      WHEN process_name GLOB 'com.android.*' THEN ts_end
      WHEN process_name = 'com.google.android.apps.nexuslauncher' THEN ts_end
      -- Some processes publish (a subset of) counters right before ending the
      -- CUJ marker slice. Updating the SQL query to consider counters up to 4ms
      -- before the CUJ ends in that case.
      ELSE MAX(ts, ts_end - 4000000)
    END AS ts_earliest_allowed_counter,
    LEAD(ts_end) OVER (PARTITION BY cuj_name ORDER BY ts_end ASC) AS ts_end_next_cuj
  FROM android_sysui_jank_cujs
)
SELECT
  cuj_id,
  _android_jank_cuj_counter_value(cuj_name, 'totalFrames', ts_earliest_allowed_counter, ts_end_next_cuj) AS total_frames,
  _android_jank_cuj_counter_value(cuj_name, 'missedFrames', ts_earliest_allowed_counter, ts_end_next_cuj) AS missed_frames,
  _android_jank_cuj_counter_value(cuj_name, 'missedAppFrames', ts_earliest_allowed_counter, ts_end_next_cuj) AS missed_app_frames,
  _android_jank_cuj_counter_value(cuj_name, 'missedSfFrames', ts_earliest_allowed_counter, ts_end_next_cuj) AS missed_sf_frames,
  _android_jank_cuj_counter_value(cuj_name, 'maxSuccessiveMissedFrames', ts_earliest_allowed_counter, ts_end_next_cuj) AS missed_frames_max_successive,
  -- convert ms to nanos to align with the unit for `dur` in the other tables
  _android_jank_cuj_counter_value(cuj_name, 'maxFrameTimeMillis', ts_earliest_allowed_counter, ts_end_next_cuj) * 1000000 AS frame_dur_max,
  _android_cuj_missed_vsyncs_for_callback(cuj_slice_name, ts_earliest_allowed_counter, ts_end_next_cuj, '*SF*') AS sf_callback_missed_frames,
  _android_cuj_missed_vsyncs_for_callback(cuj_slice_name, ts_earliest_allowed_counter, ts_end_next_cuj, '*HWUI*') AS hwui_callback_missed_frames
FROM cujs_ordered cuj;

DROP VIEW IF EXISTS android_jank_cuj_output;
CREATE PERFETTO VIEW android_jank_cuj_output AS
SELECT
  AndroidJankCujMetric(
    'cuj', (
      SELECT RepeatedField(
        AndroidJankCujMetric_Cuj(
          'id', cuj_id,
          'name', cuj_name,
          'process', process_metadata_proto(cuj.upid),
          'layer_name', layer_name,
          'ts', cuj.ts,
          'dur', cuj.dur,
          'counter_metrics', (
            SELECT AndroidJankCujMetric_Metrics(
              'total_frames', total_frames,
              'missed_frames', missed_frames,
              'missed_app_frames', missed_app_frames,
              'missed_sf_frames', missed_sf_frames,
              'missed_frames_max_successive', missed_frames_max_successive,
              'sf_callback_missed_frames', sf_callback_missed_frames,
              'hwui_callback_missed_frames', hwui_callback_missed_frames,
              'frame_dur_max', frame_dur_max)
            FROM android_jank_cuj_counter_metrics cm
            WHERE cm.cuj_id = cuj.cuj_id),
          'trace_metrics', (
            SELECT AndroidJankCujMetric_Metrics(
              'total_frames', COUNT(*),
              'missed_frames', SUM(app_missed OR sf_missed),
              'missed_app_frames', SUM(app_missed),
              'missed_sf_frames', SUM(sf_missed),
              'sf_callback_missed_frames', SUM(sf_callback_missed),
              'hwui_callback_missed_frames', SUM(hwui_callback_missed),
              'frame_dur_max', MAX(f.dur),
              'frame_dur_avg', CAST(AVG(f.dur) AS INTEGER),
              'frame_dur_p50', CAST(PERCENTILE(f.dur, 50) AS INTEGER),
              'frame_dur_p90', CAST(PERCENTILE(f.dur, 90) AS INTEGER),
              'frame_dur_p95', CAST(PERCENTILE(f.dur, 95) AS INTEGER),
              'frame_dur_p99', CAST(PERCENTILE(f.dur, 99) AS INTEGER),
              'frame_dur_ms_p50', PERCENTILE(f.dur / 1e6, 50),
              'frame_dur_ms_p90', PERCENTILE(f.dur / 1e6, 90),
              'frame_dur_ms_p95', PERCENTILE(f.dur / 1e6, 95),
              'frame_dur_ms_p99', PERCENTILE(f.dur / 1e6, 99))
            FROM android_jank_cuj_frame_trace_data f
            WHERE f.cuj_id = cuj.cuj_id),
          'timeline_metrics', (
            SELECT AndroidJankCujMetric_Metrics(
              'total_frames', COUNT(*),
              'missed_frames', SUM(app_missed OR sf_missed),
              'missed_app_frames', SUM(app_missed),
              'missed_sf_frames', SUM(sf_missed),
              'sf_callback_missed_frames', SUM(sf_callback_missed),
              'hwui_callback_missed_frames', SUM(hwui_callback_missed),
              'frame_dur_max', MAX(f.dur),
              'frame_dur_avg', CAST(AVG(f.dur) AS INTEGER),
              'frame_dur_p50', CAST(PERCENTILE(f.dur, 50) AS INTEGER),
              'frame_dur_p90', CAST(PERCENTILE(f.dur, 90) AS INTEGER),
              'frame_dur_p95', CAST(PERCENTILE(f.dur, 95) AS INTEGER),
              'frame_dur_p99', CAST(PERCENTILE(f.dur, 99) AS INTEGER),
              'frame_dur_ms_p50', PERCENTILE(f.dur / 1e6, 50),
              'frame_dur_ms_p90', PERCENTILE(f.dur / 1e6, 90),
              'frame_dur_ms_p95', PERCENTILE(f.dur / 1e6, 95),
              'frame_dur_ms_p99', PERCENTILE(f.dur / 1e6, 99))
            FROM android_jank_cuj_frame_trace_data f
            WHERE f.cuj_id = cuj.cuj_id),
          'frame', (
            SELECT RepeatedField(
              AndroidJankCujMetric_Frame(
                'frame_number', f.frame_number,
                'vsync', f.frame_id,
                'ts', f.ts,
                'dur', f.dur,
                'dur_expected', f.dur_expected,
                'app_missed', f.app_missed,
                'sf_missed', f.sf_missed,
                'sf_callback_missed', f.sf_callback_missed,
                'hwui_callback_missed', f.hwui_callback_missed))
            FROM android_jank_cuj_frame_trace_data f
            WHERE f.cuj_id = cuj.cuj_id
            ORDER BY frame_number ASC),
          'sf_frame', (
            SELECT RepeatedField(
              AndroidJankCujMetric_Frame(
                'frame_number', f.frame_number,
                'vsync', f.vsync,
                'ts', f.ts,
                'dur', f.dur,
                'dur_expected', f.dur_expected,
                'sf_missed', f.sf_missed))
            FROM android_jank_cuj_sf_frame_trace_data f
            WHERE f.cuj_id = cuj.cuj_id
            ORDER BY frame_number ASC)
        ))
      FROM android_sysui_jank_cujs cuj
      ORDER BY cuj.cuj_id ASC));
