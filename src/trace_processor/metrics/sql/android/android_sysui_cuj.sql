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

SELECT RUN_METRIC('android/process_metadata.sql');

DROP TABLE IF EXISTS android_sysui_cuj_last_cuj;
CREATE TABLE android_sysui_cuj_last_cuj AS
  SELECT
    process.name AS name,
    process.upid AS upid,
    process_metadata.metadata AS process_metadata,
    SUBSTR(slice.name, 3, LENGTH(slice.name) - 3) AS cuj_name,
    ts AS ts_start,
    ts + dur AS ts_end,
    dur AS dur
  FROM slice
  JOIN process_track ON slice.track_id = process_track.id
  JOIN process USING (upid)
  JOIN process_metadata USING (upid)
  WHERE
    slice.name GLOB 'J<*>'
    -- Filter out CUJs that are <4ms long - assuming CUJ was cancelled.
    AND slice.dur > 4e6
    AND (
      process.name GLOB 'com.google.android*'
      OR process.name GLOB 'com.android.*')
  ORDER BY ts desc
  LIMIT 1;

SELECT RUN_METRIC(
  'android/android_hwui_threads.sql',
  'table_name_prefix', 'android_sysui_cuj',
  'process_allowlist_table', 'android_sysui_cuj_last_cuj');

DROP TABLE IF EXISTS android_sysui_cuj_do_frame_slices_in_cuj;
CREATE TABLE android_sysui_cuj_do_frame_slices_in_cuj AS
SELECT
  slices.*,
  lag(slices.ts_end) OVER (ORDER BY vsync ASC) as ts_prev_frame_end
FROM android_sysui_cuj_do_frame_slices slices
JOIN android_sysui_cuj_last_cuj last_cuj
ON ts + slices.dur >= last_cuj.ts_start AND ts <= last_cuj.ts_end;

DROP TABLE IF EXISTS android_sysui_cuj_vsync_boundaries;
CREATE TABLE android_sysui_cuj_vsync_boundaries AS
SELECT MIN(vsync) as vsync_min, MAX(vsync) as vsync_max
FROM android_sysui_cuj_do_frame_slices_in_cuj;

DROP TABLE IF EXISTS android_sysui_cuj_frame_expected_timeline_events;
CREATE TABLE android_sysui_cuj_frame_expected_timeline_events AS
  SELECT
    CAST(e.name as INTEGER) as vsync,
    e.ts as ts_expected,
    e.dur as dur_expected,
    MIN(a.ts) as ts_actual_min,
    MAX(a.ts + a.dur) as ts_end_actual_max
  FROM android_sysui_cuj_last_cuj cuj
  JOIN expected_frame_timeline_slice e USING (upid)
  JOIN android_sysui_cuj_vsync_boundaries vsync
    ON CAST(e.name as INTEGER) >= vsync.vsync_min
    AND CAST(e.name as INTEGER) <= vsync.vsync_max
  JOIN actual_frame_timeline_slice a ON e.upid = a.upid AND e.name = a.name
  GROUP BY e.name, e.ts, e.dur;

DROP TABLE IF EXISTS android_sysui_cuj_frame_timeline_events;
CREATE TABLE android_sysui_cuj_frame_timeline_events AS
  SELECT
    actual.layer_name as layer_name,
    CAST(actual.name as INTEGER) as vsync,
    actual.ts as ts_actual,
    actual.dur as dur_actual,
    actual.jank_type GLOB '*App Deadline Missed*' as app_missed,
    actual.jank_type,
    actual.on_time_finish
  FROM android_sysui_cuj_last_cuj cuj
  JOIN actual_frame_timeline_slice actual USING (upid)
  JOIN android_sysui_cuj_vsync_boundaries vsync
    ON CAST(actual.name as INTEGER) >= vsync.vsync_min
    AND CAST(actual.name as INTEGER) <= vsync.vsync_max;

-- Adjust the timestamp when we consider the work on a given frame started,
-- by looking at the time the previous frame finished on the main thread
-- and the timing from the actual timeline.
-- This is to detect cases where we started doFrame late due to some other work
-- occupying the main thread.
DROP TABLE IF EXISTS android_sysui_cuj_do_frame_slices_in_cuj_adjusted;
CREATE TABLE android_sysui_cuj_do_frame_slices_in_cuj_adjusted AS
SELECT
  slices.*,
  CASE
    WHEN fte.ts_expected IS NULL
    THEN ts
    ELSE MAX(COALESCE(slices.ts_prev_frame_end, 0), fte.ts_expected)
  END as ts_adjusted
FROM android_sysui_cuj_do_frame_slices_in_cuj slices
LEFT JOIN android_sysui_cuj_frame_expected_timeline_events fte
  ON slices.vsync = fte.vsync
-- In rare cases there is a clock drift after device suspends
-- This may cause the actual/expected timeline to be misaligned with the rest
-- of the trace for a short period.
-- Do not use the timelines if it seems that this happened.
AND slices.ts >= fte.ts_actual_min - 1e6 AND slices.ts <= fte.ts_end_actual_max;

DROP TABLE IF EXISTS android_sysui_cuj_ts_boundaries;
CREATE TABLE android_sysui_cuj_ts_boundaries AS
SELECT ts, ts_end - ts as dur, ts_end FROM (
SELECT
(SELECT ts_adjusted FROM android_sysui_cuj_do_frame_slices_in_cuj_adjusted ORDER BY ts ASC LIMIT 1) as ts,
(SELECT ts FROM android_sysui_cuj_do_frame_slices_in_cuj ORDER BY ts DESC LIMIT 1) +
(SELECT dur_actual FROM android_sysui_cuj_frame_timeline_events ORDER BY vsync DESC LIMIT 1) as ts_end);

DROP VIEW IF EXISTS android_sysui_cuj_thread;
CREATE VIEW android_sysui_cuj_thread AS
SELECT
  process.name as process_name,
  thread.utid,
  thread.name
FROM thread
JOIN android_sysui_cuj_last_cuj process USING (upid);

DROP VIEW IF EXISTS android_sysui_cuj_slices_in_cuj;
CREATE VIEW android_sysui_cuj_slices_in_cuj AS
SELECT
  process_name,
  thread.utid,
  thread.name as thread_name,
  slices.*,
  slices.ts + slices.dur AS ts_end
FROM slices
JOIN thread_track ON slices.track_id = thread_track.id
JOIN android_sysui_cuj_thread thread USING (utid)
JOIN android_sysui_cuj_ts_boundaries cuj_boundaries
ON slices.ts + slices.dur >= cuj_boundaries.ts AND slices.ts <= cuj_boundaries.ts_end
WHERE slices.dur > 0;

DROP TABLE IF EXISTS android_sysui_cuj_main_thread_slices_in_cuj;
CREATE TABLE android_sysui_cuj_main_thread_slices_in_cuj AS
SELECT slices.* FROM android_sysui_cuj_main_thread_slices slices
JOIN android_sysui_cuj_ts_boundaries cuj_boundaries
ON slices.ts + slices.dur >= cuj_boundaries.ts AND slices.ts <= cuj_boundaries.ts_end;

DROP TABLE IF EXISTS android_sysui_cuj_render_thread_slices_in_cuj;
CREATE TABLE android_sysui_cuj_render_thread_slices_in_cuj AS
SELECT slices.* FROM android_sysui_cuj_render_thread_slices slices
JOIN android_sysui_cuj_ts_boundaries cuj_boundaries
ON slices.ts >= cuj_boundaries.ts AND slices.ts <= cuj_boundaries.ts_end;

DROP TABLE IF EXISTS android_sysui_cuj_draw_frame_slices_in_cuj;
CREATE TABLE android_sysui_cuj_draw_frame_slices_in_cuj AS
SELECT slices.* FROM android_sysui_cuj_draw_frame_slices slices
JOIN android_sysui_cuj_ts_boundaries cuj_boundaries
ON slices.ts >= cuj_boundaries.ts AND slices.ts <= cuj_boundaries.ts_end;

DROP TABLE IF EXISTS android_sysui_cuj_hwc_release_slices_in_cuj;
CREATE TABLE android_sysui_cuj_hwc_release_slices_in_cuj AS
SELECT slices.* FROM android_sysui_cuj_hwc_release_slices slices
JOIN android_sysui_cuj_ts_boundaries cuj_boundaries
ON slices.ts >= cuj_boundaries.ts AND slices.ts <= cuj_boundaries.ts_end;

DROP TABLE IF EXISTS android_sysui_cuj_gpu_completion_slices_in_cuj;
CREATE TABLE android_sysui_cuj_gpu_completion_slices_in_cuj AS
SELECT slices.* FROM android_sysui_cuj_gpu_completion_slices slices
JOIN android_sysui_cuj_ts_boundaries cuj_boundaries
ON slices.ts >= cuj_boundaries.ts AND slices.ts <= cuj_boundaries.ts_end;

DROP TABLE IF EXISTS android_sysui_cuj_jit_slices;
CREATE TABLE android_sysui_cuj_jit_slices AS
SELECT *
FROM android_sysui_cuj_slices_in_cuj
WHERE thread_name GLOB 'Jit thread pool*'
AND name GLOB 'JIT compiling*'
AND parent_id IS NULL;

DROP TABLE IF EXISTS android_sysui_cuj_frames;
CREATE TABLE android_sysui_cuj_frames AS
  WITH gcs_to_rt_match AS (
    SELECT
      rts.ts,
      CASE
        WHEN rtfence.name GLOB 'GPU completion fence *'
          THEN CAST(STR_SPLIT(rtfence.name, ' ', 3) AS INTEGER)
        WHEN rtfence.name GLOB 'Trace GPU completion fence *'
          THEN CAST(STR_SPLIT(rtfence.name, ' ', 4) AS INTEGER)
        ELSE NULL
      END AS idx
    FROM android_sysui_cuj_render_thread_slices_in_cuj rts
    JOIN descendant_slice(rts.id) rtfence ON rtfence.name GLOB '*GPU completion fence*'
    -- dispatchFrameCallbacks might be seen in case of
    -- drawing that happens on RT only (e.g. ripple effect)
    WHERE (rts.name GLOB 'DrawFrame*' OR rts.name = 'dispatchFrameCallbacks')
  )
  SELECT
    ROW_NUMBER() OVER (ORDER BY mts.ts) AS frame_number,
    mts.vsync as vsync,
    -- Main thread timings
    mts.ts_adjusted as ts_main_thread_start,
    mts.ts_end as ts_main_thread_end,
    mts.ts_end - mts.ts_adjusted AS dur_main_thread,
    -- RenderThread timings
    MIN(rts.ts) AS ts_render_thread_start,
    MAX(rts.ts_end) AS ts_render_thread_end,
    SUM(rts.dur) AS dur_render_thread,
    -- HWC and GPU
    SUM(gcs.ts_end - MAX(COALESCE(hwc.ts_end, 0), gcs.ts)) as dur_gcs,
    -- Overall frame timings
    COALESCE(MAX(gcs.ts_end), MAX(rts.ts_end)) AS ts_frame_end,
    COALESCE(MAX(gcs.ts_end), MAX(rts.ts_end)) - mts.ts_adjusted AS dur_frame,
    MAX(gcs_rt.idx) IS NOT NULL as drew_anything
    -- Match main thread doFrame with RT DrawFrame and optional GPU Completion
    FROM android_sysui_cuj_do_frame_slices_in_cuj_adjusted mts
    JOIN android_sysui_cuj_draw_frame_slices_in_cuj rts
      ON mts.vsync = rts.vsync
    LEFT JOIN gcs_to_rt_match gcs_rt ON gcs_rt.ts = rts.ts
    LEFT JOIN android_sysui_cuj_gpu_completion_slices_in_cuj gcs USING(idx)
    LEFT JOIN android_sysui_cuj_hwc_release_slices_in_cuj hwc USING (idx)
    GROUP BY mts.vsync, mts.ts_adjusted, mts.ts_end
    HAVING drew_anything;

DROP TABLE IF EXISTS android_sysui_cuj_missed_frames;
CREATE TABLE android_sysui_cuj_missed_frames AS
  SELECT
    f.*,
    (SELECT MAX(fte.app_missed)
     FROM android_sysui_cuj_frame_timeline_events fte
     WHERE f.vsync = fte.vsync
     AND fte.on_time_finish = 0) as app_missed
  FROM android_sysui_cuj_frames f;

DROP VIEW IF EXISTS android_sysui_cuj_frame_main_thread_bounds;
CREATE VIEW android_sysui_cuj_frame_main_thread_bounds AS
SELECT frame_number, ts_main_thread_start as ts, dur_main_thread as dur
FROM android_sysui_cuj_missed_frames
WHERE app_missed;

DROP VIEW IF EXISTS android_sysui_cuj_main_thread_state_data;
CREATE VIEW android_sysui_cuj_main_thread_state_data AS
SELECT * FROM thread_state
WHERE utid = (SELECT utid FROM android_sysui_cuj_main_thread);

DROP TABLE IF EXISTS android_sysui_cuj_main_thread_state_vt;
CREATE VIRTUAL TABLE android_sysui_cuj_main_thread_state_vt
USING span_left_join(android_sysui_cuj_frame_main_thread_bounds, android_sysui_cuj_main_thread_state_data PARTITIONED utid);

DROP TABLE IF EXISTS android_sysui_cuj_main_thread_state;
CREATE TABLE android_sysui_cuj_main_thread_state AS
  SELECT
    frame_number,
    state,
    io_wait AS io_wait,
    SUM(dur) AS dur
  FROM android_sysui_cuj_main_thread_state_vt
  GROUP BY frame_number, state, io_wait
  HAVING dur > 0;

DROP VIEW IF EXISTS android_sysui_cuj_frame_render_thread_bounds;
CREATE VIEW android_sysui_cuj_frame_render_thread_bounds AS
SELECT frame_number, ts_render_thread_start as ts, dur_render_thread as dur
FROM android_sysui_cuj_missed_frames
WHERE app_missed;

DROP VIEW IF EXISTS android_sysui_cuj_render_thread_state_data;
CREATE VIEW android_sysui_cuj_render_thread_state_data AS
SELECT * FROM thread_state
WHERE utid in (SELECT utid FROM android_sysui_cuj_render_thread);

DROP TABLE IF EXISTS android_sysui_cuj_render_thread_state_vt;
CREATE VIRTUAL TABLE android_sysui_cuj_render_thread_state_vt
USING span_left_join(android_sysui_cuj_frame_render_thread_bounds, android_sysui_cuj_render_thread_state_data PARTITIONED utid);

DROP TABLE IF EXISTS android_sysui_cuj_render_thread_state;
CREATE TABLE android_sysui_cuj_render_thread_state AS
  SELECT
    frame_number,
    state,
    io_wait AS io_wait,
    SUM(dur) AS dur
  FROM android_sysui_cuj_render_thread_state_vt
  GROUP BY frame_number, state, io_wait
  HAVING dur > 0;

DROP TABLE IF EXISTS android_sysui_cuj_main_thread_binder;
CREATE TABLE android_sysui_cuj_main_thread_binder AS
  SELECT
    f.frame_number,
    SUM(mts.dur) AS dur,
    COUNT(*) AS call_count
  FROM android_sysui_cuj_missed_frames f
  JOIN android_sysui_cuj_main_thread_slices_in_cuj mts
    ON mts.ts >= f.ts_main_thread_start AND mts.ts < f.ts_main_thread_end
  WHERE mts.name = 'binder transaction'
  AND f.app_missed
  GROUP BY f.frame_number;

DROP TABLE IF EXISTS android_sysui_cuj_sf_jank_causes;
CREATE TABLE android_sysui_cuj_sf_jank_causes AS
  WITH RECURSIVE split_jank_type(frame_number, jank_cause, remainder) AS (
    SELECT f.frame_number, "", fte.jank_type || ","
    FROM android_sysui_cuj_frames f
    JOIN android_sysui_cuj_frame_timeline_events fte ON f.vsync = fte.vsync
    UNION ALL SELECT
    frame_number,
    STR_SPLIT(remainder, ",", 0) AS jank_cause,
    TRIM(SUBSTR(remainder, INSTR(remainder, ",") + 1)) AS remainder
    FROM split_jank_type
    WHERE remainder <> "")
  SELECT frame_number, jank_cause
  FROM split_jank_type
  WHERE jank_cause NOT IN ('', 'App Deadline Missed', 'None', 'Buffer Stuffing')
  ORDER BY frame_number ASC;

DROP TABLE IF EXISTS android_sysui_cuj_missed_frames_hwui_times;
CREATE TABLE android_sysui_cuj_missed_frames_hwui_times AS
SELECT
  *,
  ts_main_thread_start AS ts,
  ts_render_thread_end - ts_main_thread_start AS dur
FROM android_sysui_cuj_missed_frames;

DROP TABLE IF EXISTS android_sysui_cuj_missed_frames_render_thread_times;
CREATE TABLE android_sysui_cuj_missed_frames_render_thread_times AS
SELECT
  *,
  ts_render_thread_start AS ts,
  dur_render_thread AS dur
FROM android_sysui_cuj_missed_frames;

DROP TABLE IF EXISTS android_sysui_cuj_jit_slices_join_table;
CREATE VIRTUAL TABLE android_sysui_cuj_jit_slices_join_table
USING span_join(android_sysui_cuj_missed_frames_hwui_times partitioned frame_number, android_sysui_cuj_jit_slices);

DROP TABLE IF EXISTS android_sysui_cuj_jank_causes;
CREATE TABLE android_sysui_cuj_jank_causes AS
  SELECT
  frame_number,
  'RenderThread - long shader_compile' AS jank_cause
  FROM android_sysui_cuj_missed_frames f
  JOIN android_sysui_cuj_render_thread_slices_in_cuj rts
    ON rts.ts >= f.ts_render_thread_start AND rts.ts < f.ts_render_thread_end
  WHERE rts.name = 'shader_compile'
  AND f.app_missed
  AND rts.dur > 8e6

  UNION ALL
  SELECT
  frame_number,
  'RenderThread - long flush layers' AS jank_cause
  FROM android_sysui_cuj_missed_frames f
  JOIN android_sysui_cuj_render_thread_slices_in_cuj rts
    ON rts.ts >= f.ts_render_thread_start AND rts.ts < f.ts_render_thread_end
  WHERE rts.name = 'flush layers'
  AND rts.dur > 8e6
  AND f.app_missed

  UNION ALL
  SELECT
  frame_number,
  'MainThread - IO wait time' AS jank_cause
  FROM android_sysui_cuj_main_thread_state
  WHERE
    ((state = 'D' OR state = 'DK') AND io_wait)
    OR (state = 'DK' AND io_wait IS NULL)
  GROUP BY frame_number
  HAVING SUM(dur) > 8e6

  UNION ALL
  SELECT
  frame_number,
  'MainThread - scheduler' AS jank_cause
  FROM android_sysui_cuj_main_thread_state
  WHERE (state = 'R' OR state = 'R+')
  GROUP BY frame_number
  HAVING SUM(dur) > 8e6
  AND SUM(dur) > (
    SELECT 0.4 * dur_main_thread
    FROM android_sysui_cuj_frames fs
    WHERE fs.frame_number = android_sysui_cuj_main_thread_state.frame_number)

  UNION ALL
  SELECT
  frame_number,
  'RenderThread - IO wait time' AS jank_cause
  FROM android_sysui_cuj_render_thread_state
  WHERE
    ((state = 'D' OR state = 'DK') AND io_wait)
    OR (state = 'DK' AND io_wait IS NULL)
  GROUP BY frame_number
  HAVING SUM(dur) > 8e6

  UNION ALL
  SELECT
  frame_number,
  'RenderThread - scheduler' AS jank_cause
  FROM android_sysui_cuj_render_thread_state
  WHERE (state = 'R' OR state = 'R+')
  GROUP BY frame_number
  HAVING SUM(dur) > 8e6
  AND SUM(dur) > (
    SELECT 0.4 * dur_render_thread
    FROM android_sysui_cuj_frames fs
    WHERE fs.frame_number = android_sysui_cuj_render_thread_state.frame_number)

  UNION ALL
  SELECT
  frame_number,
  'MainThread - binder transaction time' AS jank_cause
  FROM android_sysui_cuj_main_thread_binder
  WHERE dur > 8e6

  UNION ALL
  SELECT
  frame_number,
  'MainThread - binder calls count' AS jank_cause
  FROM android_sysui_cuj_main_thread_binder
  WHERE call_count > 8

  UNION ALL
  SELECT
  frame_number,
  'GPU completion - long completion time' AS jank_cause
  FROM android_sysui_cuj_missed_frames f
  WHERE dur_gcs > 8e6
  AND app_missed

  UNION ALL
  SELECT
  frame_number,
  'Long running time' as jank_cause
  FROM android_sysui_cuj_main_thread_state mts
  JOIN android_sysui_cuj_render_thread_state rts USING(frame_number)
  WHERE
    mts.state = 'Running'
    AND rts.state = 'Running'
    AND mts.dur + rts.dur > 15e6

  UNION ALL
  SELECT
  f.frame_number,
  'JIT compiling' as jank_cause
  FROM android_sysui_cuj_missed_frames f
  JOIN android_sysui_cuj_jit_slices_join_table jit USING (frame_number)
  WHERE f.app_missed
  GROUP BY f.frame_number
  HAVING SUM(jit.dur) > 8e6

  UNION ALL
  SELECT frame_number, jank_cause FROM android_sysui_cuj_sf_jank_causes
  GROUP BY frame_number, jank_cause;

-- TODO(b/175098682): Switch to use async slices
DROP VIEW IF EXISTS android_sysui_cuj_event;
CREATE VIEW android_sysui_cuj_event AS
 SELECT
    'slice' as track_type,
    (SELECT cuj_name FROM android_sysui_cuj_last_cuj)
        || ' - jank cause' as track_name,
    f.ts_main_thread_start as ts,
    f.dur_main_thread as dur,
    group_concat(jc.jank_cause) as slice_name
FROM android_sysui_cuj_frames f
JOIN android_sysui_cuj_jank_causes jc USING (frame_number)
GROUP BY track_type, track_name, ts, dur;

DROP VIEW IF EXISTS android_sysui_cuj_output;
CREATE VIEW android_sysui_cuj_output AS
SELECT
  AndroidSysUiCujMetrics(
      'cuj_name', cuj_name,
      'cuj_start', ts_start,
      'cuj_dur', dur,
      'process', process_metadata,
      'frames',
       (SELECT RepeatedField(
         AndroidSysUiCujMetrics_Frame(
           'number', f.frame_number,
           'vsync', f.vsync,
           'ts', f.ts_main_thread_start,
           'dur', f.dur_frame,
           'jank_cause',
              (SELECT RepeatedField(jc.jank_cause)
              FROM android_sysui_cuj_jank_causes jc WHERE jc.frame_number = f.frame_number)))
       FROM android_sysui_cuj_frames f
       ORDER BY frame_number ASC))
  FROM android_sysui_cuj_last_cuj;
