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
    slice.name LIKE 'J<%>'
    AND slice.dur > 0
    AND process.name IN (
      'com.android.systemui',
      'com.google.android.apps.nexuslauncher')
  ORDER BY ts desc
  LIMIT 1;

SELECT RUN_METRIC(
  'android/android_hwui_threads.sql',
  'table_name_prefix', 'android_sysui_cuj',
  'process_allowlist_table', 'android_sysui_cuj_last_cuj');

DROP TABLE IF EXISTS android_sysui_cuj_main_thread_slices_in_cuj;
CREATE TABLE android_sysui_cuj_main_thread_slices_in_cuj AS
SELECT slices.* FROM android_sysui_cuj_main_thread_slices slices
JOIN android_sysui_cuj_last_cuj last_cuj
ON ts >= last_cuj.ts_start AND ts <= last_cuj.ts_end;

DROP TABLE IF EXISTS android_sysui_cuj_render_thread_slices_in_cuj;
CREATE TABLE android_sysui_cuj_render_thread_slices_in_cuj AS
SELECT slices.* FROM android_sysui_cuj_render_thread_slices slices
JOIN android_sysui_cuj_last_cuj last_cuj
ON ts >= last_cuj.ts_start AND ts <= last_cuj.ts_end;

DROP TABLE IF EXISTS android_sysui_cuj_hwc_release_slices_in_cuj;
CREATE TABLE android_sysui_cuj_hwc_release_slices_in_cuj AS
SELECT slices.* FROM android_sysui_cuj_hwc_release_slices slices
JOIN android_sysui_cuj_last_cuj last_cuj
ON ts >= last_cuj.ts_start AND ts <= last_cuj.ts_end;

DROP TABLE IF EXISTS android_sysui_cuj_gpu_completion_slices_in_cuj;
CREATE TABLE android_sysui_cuj_gpu_completion_slices_in_cuj AS
SELECT slices.* FROM android_sysui_cuj_gpu_completion_slices slices
JOIN android_sysui_cuj_last_cuj last_cuj
ON ts >= last_cuj.ts_start AND ts <= last_cuj.ts_end;

DROP TABLE IF EXISTS android_sysui_cuj_frames;
CREATE TABLE android_sysui_cuj_frames AS
  WITH gcs_to_rt_match AS (
    -- Match GPU Completion with the last RT slice before it
    SELECT
      gcs.ts as gcs_ts,
      gcs.ts_end as gcs_ts_end,
      gcs.dur as gcs_dur,
      gcs.idx as idx,
      MAX(rts.ts) as rts_ts
    FROM android_sysui_cuj_gpu_completion_slices_in_cuj gcs
    JOIN android_sysui_cuj_render_thread_slices_in_cuj rts ON rts.ts < gcs.ts
    -- dispatchFrameCallbacks might be seen in case of
    -- drawing that happens on RT only (e.g. ripple effect)
    WHERE (rts.name = 'DrawFrame' OR rts.name = 'dispatchFrameCallbacks')
    GROUP BY gcs.ts, gcs.ts_end, gcs.dur, gcs.idx
  ),
  frame_boundaries AS (
    -- Match main thread doFrame with RT DrawFrame and optional GPU Completion
    SELECT
      mts.ts as mts_ts,
      mts.ts_end as mts_ts_end,
      mts.dur as mts_dur,
      MAX(gcs_rt.gcs_ts) as gcs_ts_start,
      MAX(gcs_rt.gcs_ts_end) as gcs_ts_end
    FROM android_sysui_cuj_main_thread_slices_in_cuj mts
    JOIN android_sysui_cuj_render_thread_slices_in_cuj rts
      ON mts.ts < rts.ts AND mts.ts_end >= rts.ts
    LEFT JOIN gcs_to_rt_match gcs_rt ON gcs_rt.rts_ts = rts.ts
    WHERE mts.name = 'Choreographer#doFrame' AND rts.name = 'DrawFrame'
    GROUP BY mts.ts, mts.ts_end, mts.dur
  )
  SELECT
    ROW_NUMBER() OVER (ORDER BY f.mts_ts) AS frame_number,
    f.mts_ts as ts_main_thread_start,
    f.mts_ts_end as ts_main_thread_end,
    f.mts_dur AS dur_main_thread,
    MIN(rts.ts) AS ts_render_thread_start,
    MAX(rts.ts_end) AS ts_render_thread_end,
    SUM(rts.dur) AS dur_render_thread,
    MAX(gcs_rt.gcs_ts_end) AS ts_frame_end,
    MAX(gcs_rt.gcs_ts_end) - f.mts_ts AS dur_frame,
    SUM(gcs_rt.gcs_ts_end - MAX(COALESCE(hwc.ts_end, 0), gcs_rt.gcs_ts)) as dur_gcs,
    COUNT(DISTINCT(rts.ts)) as draw_frames,
    COUNT(DISTINCT(gcs_rt.gcs_ts)) as gpu_completions
  FROM frame_boundaries f
  JOIN android_sysui_cuj_render_thread_slices_in_cuj rts
    ON f.mts_ts < rts.ts AND f.mts_ts_end >= rts.ts
  LEFT JOIN gcs_to_rt_match gcs_rt
    ON rts.ts = gcs_rt.rts_ts
  LEFT JOIN android_sysui_cuj_hwc_release_slices_in_cuj hwc USING (idx)
  WHERE rts.name = 'DrawFrame'
  GROUP BY f.mts_ts
  HAVING gpu_completions >= 1;

DROP VIEW IF EXISTS android_sysui_cuj_frame_main_thread_bounds;
CREATE VIEW android_sysui_cuj_frame_main_thread_bounds AS
SELECT frame_number, ts_main_thread_start as ts, dur_main_thread as dur
FROM android_sysui_cuj_frames;

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
FROM android_sysui_cuj_frames;

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
  FROM android_sysui_cuj_frames f
  JOIN android_sysui_cuj_main_thread_slices_in_cuj mts
    ON mts.ts >= f.ts_main_thread_start AND mts.ts < f.ts_main_thread_end
  WHERE mts.name = 'binder transaction'
  GROUP BY f.frame_number;

DROP TABLE IF EXISTS android_sysui_cuj_jank_causes;
CREATE TABLE android_sysui_cuj_jank_causes AS
  SELECT
  frame_number,
  'RenderThread - long shader_compile' AS jank_cause
  FROM android_sysui_cuj_frames f
  JOIN android_sysui_cuj_render_thread_slices_in_cuj rts
    ON rts.ts >= f.ts_render_thread_start AND rts.ts < f.ts_render_thread_end
  WHERE rts.name = 'shader_compile'
  AND rts.dur > 8000000

  UNION ALL
  SELECT
  frame_number,
  'RenderThread - long flush layers' AS jank_cause
  FROM android_sysui_cuj_frames f
  JOIN android_sysui_cuj_render_thread_slices_in_cuj rts
    ON rts.ts >= f.ts_render_thread_start AND rts.ts < f.ts_render_thread_end
  WHERE rts.name = 'flush layers'
  AND rts.dur > 8000000

  UNION ALL
  SELECT
  frame_number,
  'MainThread - IO wait time' AS jank_cause
  FROM android_sysui_cuj_main_thread_state
  WHERE
    ((state = 'D' OR state = 'DK') AND io_wait)
    OR (state = 'DK' AND io_wait IS NULL)
  GROUP BY frame_number
  HAVING SUM(dur) > 8000000

  UNION ALL
  SELECT
  frame_number,
  'MainThread - scheduler' AS jank_cause
  FROM android_sysui_cuj_main_thread_state
  WHERE (state = 'R' OR state = 'R+')
  GROUP BY frame_number
  HAVING SUM(dur) > 8000000

  UNION ALL
  SELECT
  frame_number,
  'RenderThread - IO wait time' AS jank_cause
  FROM android_sysui_cuj_render_thread_state
  WHERE
    ((state = 'D' OR state = 'DK') AND io_wait)
    OR (state = 'DK' AND io_wait IS NULL)
  GROUP BY frame_number
  HAVING SUM(dur) > 8000000

  UNION ALL
  SELECT
  frame_number,
  'RenderThread - scheduler' AS jank_cause
  FROM android_sysui_cuj_render_thread_state
  WHERE (state = 'R' OR state = 'R+')
  GROUP BY frame_number
  HAVING SUM(dur) > 8000000

  UNION ALL
  SELECT
  frame_number,
  'MainThread - binder transaction time' AS jank_cause
  FROM android_sysui_cuj_main_thread_binder
  WHERE dur > 8000000

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
  FROM android_sysui_cuj_frames f
  WHERE dur_gcs > 8000000

  UNION ALL
  SELECT
  frame_number,
  'Long running time' as jank_cause
  FROM android_sysui_cuj_main_thread_state mts
  JOIN android_sysui_cuj_render_thread_state rts USING(frame_number)
  WHERE
    mts.state = 'Running'
    AND rts.state = 'Running'
    AND mts.dur + rts.dur > 15000000;

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
           'ts', f.ts_main_thread_start,
           'dur', f.dur_frame,
           'jank_cause',
              (SELECT RepeatedField(jc.jank_cause)
              FROM android_sysui_cuj_jank_causes jc WHERE jc.frame_number = f.frame_number)))
       FROM android_sysui_cuj_frames f
       ORDER BY frame_number ASC))
  FROM android_sysui_cuj_last_cuj;
