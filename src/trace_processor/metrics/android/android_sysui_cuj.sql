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

CREATE VIEW IF NOT EXISTS android_sysui_cuj_output AS

WITH last_cuj AS (
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
  LIMIT 1
),
render_thread AS (
  SELECT thread.*, last_cuj.ts_start as ts_cuj_start, last_cuj.ts_end as ts_cuj_end
  FROM thread
  JOIN last_cuj USING (upid)
  WHERE thread.name = 'RenderThread'
),
gpu_completion_thread AS (
  SELECT thread.*, last_cuj.ts_start as ts_cuj_start, last_cuj.ts_end as ts_cuj_end
  FROM thread
  JOIN last_cuj USING (upid)
  WHERE thread.name = 'GPU completion'
),
main_thread_slices AS (
  SELECT slice.*, ts + dur AS ts_end
  FROM slice
  JOIN last_cuj ON slice.track_id = last_cuj.main_thread_track_id
  WHERE ts >= last_cuj.ts_start AND ts <= last_cuj.ts_end
),
render_thread_slices AS (
  SELECT slice.*, ts + dur AS ts_end
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN render_thread USING (utid)
  WHERE ts >= ts_cuj_start AND ts <= ts_cuj_end
),
gpu_completion_slices AS (
  SELECT slice.*, ts + dur AS ts_end
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN gpu_completion_thread USING (utid)
  WHERE
    slice.name LIKE 'waiting for GPU completion %'
    AND ts >= ts_cuj_start AND ts <= ts_cuj_end
),
frames AS (
  SELECT
    ROW_NUMBER() OVER (ORDER BY mts.ts) AS frame_number,
    mts.ts AS ts_frame_start,
    MIN(gcs.ts_end) AS ts_frame_end,
    mts.ts AS ts_main_thread_start,
    mts.ts_end AS ts_main_thread_end,
    mts.dur AS dur_main_thread,
    rts.ts AS ts_render_thread_start,
    rts.ts_end AS ts_render_thread_end,
    rts.dur AS dur_render_thread,
    (MIN(gcs.ts_end) - mts.ts) AS dur
  FROM main_thread_slices mts
  JOIN render_thread_slices rts
    ON mts.ts < rts.ts AND rts.name = 'DrawFrame'
  JOIN gpu_completion_slices gcs ON rts.ts < gcs.ts
  WHERE mts.name = 'Choreographer#doFrame'
  GROUP BY mts.ts
),
main_thread_state AS (
  SELECT
    f.frame_number,
    mts.state,
    SUM(mts.dur) / 1000000 AS duration_millis,
    SUM(mts.io_wait) AS io_wait
  FROM frames f
  JOIN thread_state mts
    ON mts.ts >= f.ts_main_thread_start AND mts.ts < f.ts_main_thread_end
  WHERE mts.utid = (SELECT main_thread_utid FROM last_cuj)
  GROUP BY f.frame_number, mts.state
  HAVING duration_millis > 0
),
render_thread_state AS (
  SELECT
    f.frame_number,
    rts.state,
    SUM(rts.dur) / 1000000 AS duration_millis,
    SUM(rts.io_wait) AS io_wait
  FROM frames f
  JOIN thread_state rts
    ON rts.ts >= f.ts_render_thread_start AND rts.ts < f.ts_render_thread_end
  WHERE rts.utid in (SELECT utid FROM render_thread)
  GROUP BY f.frame_number, rts.state
  HAVING duration_millis > 0
),
main_thread_binder AS (
  SELECT
    f.frame_number,
    SUM(mts.dur) / 1000000 AS duration_millis,
    COUNT(*) AS call_count
  FROM frames f
  JOIN main_thread_slices mts
    ON mts.ts >= f.ts_main_thread_start AND mts.ts < f.ts_main_thread_end
  WHERE mts.name = 'binder transaction'
  GROUP BY f.frame_number
),
jank_causes AS (
  SELECT
  frame_number,
  'RenderThread - long shader_compile' AS jank_cause
  FROM frames f
  JOIN render_thread_slices rts
    ON rts.ts >= f.ts_render_thread_start AND rts.ts < f.ts_render_thread_end
  WHERE rts.name = 'shader_compile'
  AND rts.dur / 1000000 > 8

  UNION ALL
  SELECT
  frame_number,
  'RenderThread - long flush layers' AS jank_cause
  FROM frames f
  JOIN render_thread_slices rts
    ON rts.ts >= f.ts_render_thread_start AND rts.ts < f.ts_render_thread_end
  WHERE rts.name = 'flush layers'
  AND rts.dur / 1000000 > 8

  UNION ALL
  SELECT
  frame_number,
  'MainThread - IO wait time' AS jank_cause
  FROM main_thread_state
  WHERE state = 'DK' AND duration_millis > 8

  UNION ALL
  SELECT
  frame_number,
  'RenderThread - IO wait time' AS jank_cause
  FROM render_thread_state
  WHERE state = 'DK' AND duration_millis > 8

  UNION ALL
  SELECT
  frame_number,
  'MainThread - binder transaction time' AS jank_cause
  FROM main_thread_binder
  WHERE duration_millis > 8

  UNION ALL
  SELECT
  frame_number,
  'MainThread - binder calls count' AS jank_cause
  FROM main_thread_binder
  WHERE call_count > 8
)
SELECT
  AndroidSysUiCujMetrics(
      'frames',
       (SELECT RepeatedField(
         AndroidSysUiCujMetrics_Frame(
           'number', f.frame_number,
           'ts', f.ts_frame_start,
           'dur', f.dur,
           'jank_cause',
              (SELECT RepeatedField(jc.jank_cause)
              FROM jank_causes jc WHERE jc.frame_number = f.frame_number)))
       FROM frames f
       ORDER BY frame_number ASC))
