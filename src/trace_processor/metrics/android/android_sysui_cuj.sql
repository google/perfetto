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

CREATE TABLE IF NOT EXISTS android_sysui_cuj_last_cuj AS
  SELECT
    process.name AS process_name,
    process.upid AS upid,
    main_thread.utid AS main_thread_utid,
    main_thread.name AS main_thread_name,
    thread_track.id AS main_thread_track_id,
    slice.name AS slice_name,
    ts AS ts_start,
    ts + dur AS ts_end
  FROM slice
  JOIN process_track ON slice.track_id = process_track.id
  JOIN process USING (upid)
  JOIN thread AS main_thread ON main_thread.upid = process.upid AND main_thread.is_main_thread
  JOIN thread_track USING (utid)
  WHERE
    slice.name LIKE 'Cuj<%>'
    AND slice.dur > 0
    AND process.name IN (
      'com.android.systemui',
      'com.google.android.apps.nexuslauncher')
  ORDER BY ts desc
  LIMIT 1;

CREATE VIEW IF NOT EXISTS android_sysui_cuj_render_thread AS
  SELECT thread.*, last_cuj.ts_start as ts_cuj_start, last_cuj.ts_end as ts_cuj_end
  FROM thread
  JOIN android_sysui_cuj_last_cuj last_cuj USING (upid)
  WHERE thread.name = 'RenderThread';

CREATE VIEW IF NOT EXISTS android_sysui_cuj_gpu_completion_thread AS
  SELECT thread.*, last_cuj.ts_start as ts_cuj_start, last_cuj.ts_end as ts_cuj_end
  FROM thread
  JOIN android_sysui_cuj_last_cuj last_cuj USING (upid)
  WHERE thread.name = 'GPU completion';

CREATE VIEW IF NOT EXISTS android_sysui_cuj_hwc_release_thread AS
  SELECT thread.*, last_cuj.ts_start as ts_cuj_start, last_cuj.ts_end as ts_cuj_end
  FROM thread
  JOIN android_sysui_cuj_last_cuj last_cuj USING (upid)
  WHERE thread.name = 'HWC release';

CREATE TABLE IF NOT EXISTS android_sysui_cuj_main_thread_slices AS
  SELECT slice.*, ts + dur AS ts_end
  FROM slice
  JOIN android_sysui_cuj_last_cuj last_cuj
    ON slice.track_id = last_cuj.main_thread_track_id
  WHERE ts >= last_cuj.ts_start AND ts <= last_cuj.ts_end;

CREATE TABLE IF NOT EXISTS android_sysui_cuj_render_thread_slices AS
  SELECT slice.*, ts + dur AS ts_end
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN android_sysui_cuj_render_thread USING (utid)
  WHERE ts >= ts_cuj_start AND ts <= ts_cuj_end;

CREATE TABLE IF NOT EXISTS android_sysui_cuj_gpu_completion_slices AS
  SELECT
    slice.*,
    ts + dur AS ts_end,
    -- Extracts 1234 from 'waiting for GPU completion 1234'
    CAST(STR_SPLIT(slice.name, ' ', 4) AS INTEGER) as idx
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN android_sysui_cuj_gpu_completion_thread USING (utid)
  WHERE
    slice.name LIKE 'waiting for GPU completion %'
    AND ts >= ts_cuj_start AND ts <= ts_cuj_end;

CREATE TABLE IF NOT EXISTS android_sysui_cuj_hwc_release_slices AS
  SELECT
    slice.*,
    ts + dur as ts_end,
    -- Extracts 1234 from 'waiting for HWC release 1234'
    CAST(STR_SPLIT(slice.name, ' ', 4) AS INTEGER) as idx
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN android_sysui_cuj_hwc_release_thread USING (utid)
  WHERE
    slice.name LIKE 'waiting for HWC release %'
    AND ts >= ts_cuj_start AND ts <= ts_cuj_end;

CREATE TABLE IF NOT EXISTS android_sysui_cuj_frames AS
  WITH gcs_to_rt_match AS (
    -- Match GPU Completion with the last RT slice before it
    SELECT
      gcs.ts as gcs_ts,
      gcs.ts_end as gcs_ts_end,
      gcs.dur as gcs_dur,
      gcs.idx as idx,
      MAX(rts.ts) as rts_ts
    FROM android_sysui_cuj_gpu_completion_slices gcs
    JOIN android_sysui_cuj_render_thread_slices rts ON rts.ts < gcs.ts
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
    FROM android_sysui_cuj_main_thread_slices mts
    JOIN android_sysui_cuj_render_thread_slices rts
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
  JOIN android_sysui_cuj_render_thread_slices rts
    ON f.mts_ts < rts.ts AND f.mts_ts_end >= rts.ts
  LEFT JOIN gcs_to_rt_match gcs_rt
    ON rts.ts = gcs_rt.rts_ts
  LEFT JOIN android_sysui_cuj_hwc_release_slices hwc USING (idx)
  WHERE rts.name = 'DrawFrame'
  GROUP BY f.mts_ts
  HAVING gpu_completions >= 1;

CREATE TABLE IF NOT EXISTS android_sysui_cuj_main_thread_state AS
  SELECT
    f.frame_number,
    mts.state,
    SUM(mts.dur) AS dur,
    SUM(mts.io_wait) AS io_wait
  FROM android_sysui_cuj_frames f
  JOIN thread_state mts
    ON mts.ts >= f.ts_main_thread_start AND mts.ts < f.ts_main_thread_end
  WHERE mts.utid = (SELECT main_thread_utid FROM android_sysui_cuj_last_cuj)
  GROUP BY f.frame_number, mts.state
  HAVING mts.dur > 0;

CREATE TABLE IF NOT EXISTS android_sysui_cuj_render_thread_state AS
  SELECT
    f.frame_number,
    rts.state,
    SUM(rts.dur) AS dur,
    SUM(rts.io_wait) AS io_wait
  FROM android_sysui_cuj_frames f
  JOIN thread_state rts
    ON rts.ts >= f.ts_render_thread_start AND rts.ts < f.ts_render_thread_end
  WHERE rts.utid in (SELECT utid FROM android_sysui_cuj_render_thread)
  GROUP BY f.frame_number, rts.state
  HAVING rts.dur > 0;

CREATE TABLE IF NOT EXISTS android_sysui_cuj_main_thread_binder AS
  SELECT
    f.frame_number,
    SUM(mts.dur) AS dur,
    COUNT(*) AS call_count
  FROM android_sysui_cuj_frames f
  JOIN android_sysui_cuj_main_thread_slices mts
    ON mts.ts >= f.ts_main_thread_start AND mts.ts < f.ts_main_thread_end
  WHERE mts.name = 'binder transaction'
  GROUP BY f.frame_number;

CREATE TABLE IF NOT EXISTS android_sysui_cuj_jank_causes AS
  SELECT
  frame_number,
  'RenderThread - long shader_compile' AS jank_cause
  FROM android_sysui_cuj_frames f
  JOIN android_sysui_cuj_render_thread_slices rts
    ON rts.ts >= f.ts_render_thread_start AND rts.ts < f.ts_render_thread_end
  WHERE rts.name = 'shader_compile'
  AND rts.dur > 8000000

  UNION ALL
  SELECT
  frame_number,
  'RenderThread - long flush layers' AS jank_cause
  FROM android_sysui_cuj_frames f
  JOIN android_sysui_cuj_render_thread_slices rts
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
CREATE VIEW IF NOT EXISTS android_sysui_cuj_event AS
 SELECT
    'slice' as track_type,
    (SELECT slice_name FROM android_sysui_cuj_last_cuj)
        || ' - jank cause' as track_name,
    f.ts_main_thread_start as ts,
    f.dur_main_thread as dur,
    group_concat(jc.jank_cause) as slice_name
FROM android_sysui_cuj_frames f
JOIN android_sysui_cuj_jank_causes jc USING (frame_number)
GROUP BY track_type, track_name, ts, dur;

CREATE VIEW IF NOT EXISTS android_sysui_cuj_output AS
SELECT
  AndroidSysUiCujMetrics(
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
       ORDER BY frame_number ASC));
