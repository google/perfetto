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

-- Creates a table that matches CUJ counters with the correct CUJs.
-- After the CUJ ends FrameTracker emits counters with the number of total
-- frames, missed frames, longest frame duration, etc.
-- The same numbers are also reported by FrameTracker to statsd.
SELECT RUN_METRIC('android/jank/internal/counters.sql');

SELECT RUN_METRIC('android/process_metadata.sql');
INCLUDE PERFETTO MODULE android.frames.jank_type;
INCLUDE PERFETTO MODULE android.frames.timeline;

-- Table captures all frames within a CUJ boundary.
DROP TABLE IF EXISTS _android_jank_cuj_frames;
CREATE PERFETTO TABLE _android_jank_cuj_frames AS
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
      cuj.upid,
      frame.layer_name AS frame_layer_name,
      frame.frame_id,
      actual_frame.display_frame_token,
      cuj.cuj_id,
      frame.ts AS frame_ts,
      (
        frame.ts + frame.dur
      ) AS ts_end,
      jank_type,
      on_time_finish,
      sf_callback_missed,
      hwui_callback_missed
    FROM android_frames_layers AS frame
    JOIN android_sysui_jank_cujs AS cuj
      ON frame.layer_id = cuj.layer_id AND frame.ui_thread_utid = cuj.ui_thread
    JOIN actual_timeline_with_vsync AS actual_frame
      ON frame.frame_id = actual_frame.vsync AND cuj.layer_id = CAST(str_split(actual_frame.layer_name, '#', 1) AS INTEGER)
    LEFT JOIN _vsync_missed_callback AS missed_callback USING(vsync)
    -- Check whether the frame_id falls within the begin and end vsync of the cuj.
    -- Also check if the frame start or end timestamp falls within the cuj boundary.
    WHERE frame.frame_id >= begin_vsync AND frame.frame_id <= end_vsync
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
      COALESCE(MAX(e.dur), 16600000) AS dur_expected
    FROM frames_in_cuj vsync_boundary
    JOIN expected_timeline_with_vsync e
      ON e.upid = vsync_boundary.upid  AND e.vsync = vsync_boundary.frame_id
    GROUP BY cuj_id, e.vsync, e.ts;

-- Table captures all Choreographer#doFrame within a CUJ boundary.
DROP TABLE IF EXISTS _android_jank_cuj_do_frames;
CREATE PERFETTO TABLE _android_jank_cuj_do_frames AS
WITH do_frame_slice AS (
  SELECT
    frame_id,
    do_frame.ts,
    upid,
    slice.ts + slice.dur AS ts_end,
    slice.track_id
  FROM android_frames_choreographer_do_frame do_frame
  JOIN slice USING(id)
)
SELECT
  cuj.cuj_id,
  cuj.ui_thread,
  do_frame.*
FROM thread
JOIN android_sysui_jank_cujs cuj USING (upid)
JOIN thread_track USING(utid)
JOIN do_frame_slice do_frame
  ON do_frame.ts_end >= cuj.ts AND do_frame.ts <= cuj.ts_end AND do_frame.upid = cuj.upid
  AND thread_track.id = do_frame.track_id
WHERE
  ((cuj.ui_thread IS NULL AND thread.is_main_thread)
  -- Some CUJs use a dedicated thread for Choreographer callbacks
  OR (cuj.ui_thread = thread.utid))
  AND frame_id > 0
  AND (frame_id >= begin_vsync OR begin_vsync is NULL)
  AND (frame_id <= end_vsync OR end_vsync is NULL);

DROP TABLE IF EXISTS android_jank_cuj_frame_new;
CREATE PERFETTO TABLE android_jank_cuj_frame_new AS
WITH do_frame_ordered AS (
  SELECT
    *,
    -- ts_end of the previous do_frame, or -1 if no previous do_frame found
    COALESCE(LAG(ts_end) OVER (PARTITION BY cuj_id ORDER BY frame_id ASC), -1) AS ts_prev_do_frame_end
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
   LEFT JOIN _android_jank_cuj_frames timeline USING(cuj_id, frame_id)
)
select
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

DROP TABLE IF EXISTS android_jank_cuj_sf_main_thread;
CREATE PERFETTO TABLE android_jank_cuj_sf_main_thread AS
SELECT upid, utid, thread.name, thread_track.id AS track_id
FROM thread
JOIN _android_jank_cuj_sf_process sf_process USING (upid)
JOIN thread_track USING (utid)
WHERE thread.is_main_thread;

DROP TABLE IF EXISTS android_jank_cuj_app_sf_match;
CREATE PERFETTO TABLE android_jank_cuj_app_sf_match AS
SELECT
 cuj_id,
   do_frame.upid AS app_upid,
   app_vsync,
   app_sf_match.sf_upid,
   app_sf_match.sf_vsync
FROM _android_jank_cuj_do_frames do_frame JOIN android_app_to_sf_vsync_match app_sf_match
  ON do_frame.frame_id = app_sf_match.app_vsync AND do_frame.upid = app_sf_match.app_upid;

CREATE OR REPLACE PERFETTO FUNCTION find_android_jank_cuj_sf_main_thread_slice(
  slice_name_glob STRING)
RETURNS TABLE(
  cuj_id INT, utid INT, vsync INT, id INT,
  name STRING, ts LONG, dur LONG, ts_end LONG)
AS
WITH sf_vsync AS (
  SELECT DISTINCT cuj_id, sf_vsync AS vsync
  FROM android_jank_cuj_app_sf_match)
SELECT
  cuj_id,
  utid,
  sf_vsync.vsync,
  slice.id,
  slice.name,
  slice.ts,
  slice.dur,
  slice.ts + slice.dur AS ts_end
FROM slice
JOIN android_jank_cuj_sf_main_thread main_thread USING (track_id)
JOIN sf_vsync
  ON CAST(STR_SPLIT(slice.name, " ", 1) AS INTEGER) = sf_vsync.vsync
WHERE slice.name GLOB $slice_name_glob AND slice.dur > 0
ORDER BY cuj_id, vsync;

DROP TABLE IF EXISTS android_jank_cuj_sf_commit_slice;
CREATE PERFETTO TABLE android_jank_cuj_sf_commit_slice AS
SELECT * FROM FIND_ANDROID_JANK_CUJ_SF_MAIN_THREAD_SLICE('commit *');

DROP TABLE IF EXISTS android_jank_cuj_sf_composite_slice;
CREATE PERFETTO TABLE android_jank_cuj_sf_composite_slice AS
SELECT * FROM FIND_ANDROID_JANK_CUJ_SF_MAIN_THREAD_SLICE('composite *');

-- Older builds do not have the commit/composite but onMessageInvalidate instead
DROP TABLE IF EXISTS android_jank_cuj_sf_on_message_invalidate_slice;
CREATE PERFETTO TABLE android_jank_cuj_sf_on_message_invalidate_slice AS
SELECT * FROM FIND_ANDROID_JANK_CUJ_SF_MAIN_THREAD_SLICE('onMessageInvalidate *');

DROP TABLE IF EXISTS android_jank_cuj_sf_frame_new;
CREATE PERFETTO TABLE android_jank_cuj_sf_frame_new AS
-- Join `commit` and `composite` slices using vsync IDs.
-- We treat the two slices as a single "fake slice" that starts when `commit` starts, and ends
-- when `composite` ends.
WITH fake_commit_composite_slice AS (
  SELECT
    cuj_id,
    commit_slice.utid,
    vsync,
    commit_slice.ts,
    composite_slice.ts_end,
    composite_slice.ts_end - commit_slice.ts AS dur
  FROM android_jank_cuj_sf_commit_slice commit_slice
  JOIN android_jank_cuj_sf_composite_slice composite_slice USING(cuj_id, vsync)
),
-- As older builds will not have separate commit/composite slices for each frame, but instead
-- a single `onMessageInvalidate`, we UNION ALL the two tables. Exactly one of them should
-- have data.
main_thread_slice AS (
  SELECT utid, cuj_id, vsync, ts, dur, ts_end FROM fake_commit_composite_slice
  UNION ALL
  SELECT utid, cuj_id, vsync, ts, dur, ts_end FROM android_jank_cuj_sf_on_message_invalidate_slice
),
sf_mt_boundary AS (
SELECT
  cuj_id,
  utid,
  vsync,
  expected_timeline.ts,
  main_thread_slice.ts AS ts_main_thread_start,
  main_thread_slice.ts_end,
  main_thread_slice.ts_end - expected_timeline.ts AS dur
FROM expected_frame_timeline_slice expected_timeline
JOIN _android_jank_cuj_sf_process USING (upid)
JOIN main_thread_slice
  ON main_thread_slice.vsync = CAST(expected_timeline.name AS INTEGER)
  ),
android_jank_cuj_sf_frame_base AS (
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
      -- See similar expression in android_jank_cuj_frame_timeline.
      COALESCE(expected_timeline.dur, 16600000) AS dur_expected
    FROM sf_mt_boundary boundary
    JOIN _android_jank_cuj_sf_process sf_process
    JOIN actual_frame_timeline_slice actual_timeline
      ON actual_timeline.upid = sf_process.upid
        AND boundary.vsync = CAST(actual_timeline.name AS INTEGER)
    JOIN _android_jank_cuj_frames ft
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
            FROM android_jank_cuj_frame_new f
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
            FROM android_jank_cuj_frame_new f
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
            FROM android_jank_cuj_frame_new f
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
            FROM android_jank_cuj_sf_frame_new f
            WHERE f.cuj_id = cuj.cuj_id
            ORDER BY frame_number ASC)
        ))
      FROM android_sysui_jank_cujs cuj
      ORDER BY cuj.cuj_id ASC));
