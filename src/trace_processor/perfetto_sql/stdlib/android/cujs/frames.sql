--
-- Copyright 2025 The Android Open Source Project
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

INCLUDE PERFETTO MODULE android.cujs.base;

INCLUDE PERFETTO MODULE android.cujs.slices;

INCLUDE PERFETTO MODULE android.cujs.boundaries;

INCLUDE PERFETTO MODULE android.frames.jank_type;

INCLUDE PERFETTO MODULE android.frames.timeline;

-- This table serves as a comprehensive per-frame analysis of UI Jank for CUJs.
-- It correlates individual frame timelines with the specific time boundaries of a CUJ
-- to determine whether the app or SurfaceFlinger frame missed their respective deadlines.
CREATE PERFETTO TABLE android_jank_cuj_frame_timeline (
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- vsync id of the frame.
  vsync LONG,
  -- Whether app frame missed deadline.
  app_missed LONG,
  -- Whether surfaceflinger frame missed deadline.
  sf_missed LONG,
  -- Whether surfaceflinger callback was missed.
  sf_callback_missed LONG,
  -- Whether HWUI callback was missed.
  hwui_callback_missed LONG,
  -- Whether frame finished on time.
  on_time_finish LONG,
  -- end ts of frame based on actual timeline.
  ts_end_actual TIMESTAMP,
  -- actual frame duration.
  dur LONG,
  -- expected frame duration.
  dur_expected LONG,
  -- number of distinct layers for frame.
  number_of_layers_for_frame LONG,
  -- layer name for frame.
  frame_layer_name STRING
) AS
WITH
  actual_timeline_with_vsync AS (
    SELECT
      *,
      CAST(name AS INTEGER) AS vsync
    FROM actual_frame_timeline_slice
    WHERE
      dur > 0
  )
SELECT
  cuj_id,
  vsync,
  -- We use MAX to check if at least one of the layers jank_type matches the pattern
  max(android_is_app_jank_type(jank_type)) AS app_missed,
  -- We use MAX to check if at least one of the layers jank_type matches the pattern
  max(android_is_sf_jank_type(jank_type)) AS sf_missed,
  coalesce(max(sf_callback_missed), 0) AS sf_callback_missed,
  coalesce(max(hwui_callback_missed), 0) AS hwui_callback_missed,
  -- We use MIN to check if ALL layers finished on time
  min(on_time_finish) AS on_time_finish,
  max(timeline.ts + timeline.dur) AS ts_end_actual,
  max(timeline.dur) AS dur,
  -- At the moment of writing we expect to see at most one expected_frame_timeline_slice
  -- for a given vsync but using MAX here in case this changes in the future.
  -- In case expected timeline is missing, as a fallback we use the typical frame deadline
  -- for 60Hz.
  coalesce(max(expected.dur), 16600000) AS dur_expected,
  count(DISTINCT timeline.layer_name) AS number_of_layers_for_frame,
  -- we use MAX to get at least one of the frame's layer names
  max(timeline.layer_name) AS frame_layer_name
FROM _android_jank_cuj_vsync_boundary AS boundary
JOIN actual_timeline_with_vsync AS timeline
  ON vsync >= vsync_min AND vsync <= vsync_max
LEFT JOIN expected_frame_timeline_slice AS expected
  ON expected.upid = timeline.upid AND expected.name = timeline.name
LEFT JOIN _vsync_missed_callback AS missed_callback
  USING (vsync)
WHERE
  boundary.layer_id IS NULL
  OR (
    timeline.layer_name GLOB '*#*'
    AND boundary.layer_id = CAST(str_split(timeline.layer_name, '#', 1) AS INTEGER)
  )
GROUP BY
  cuj_id,
  vsync;

-- Table captures the layer name associated with each CUJ.
CREATE PERFETTO TABLE android_jank_cuj_layer_name (
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- layer name associated with CUJ.
  layer_name STRING
) AS
SELECT
  cuj_id,
  max(frame_layer_name) AS layer_name
FROM android_jank_cuj_frame_timeline AS timeline
GROUP BY
  cuj_id
HAVING
  max(number_of_layers_for_frame) = 1;

-- Matches slices and boundaries to compute estimated frame boundaries across
-- all threads. Joins with the actual timeline to figure out which frames missed
-- the deadline and whether the app process or SF are at fault.
CREATE PERFETTO TABLE android_jank_cuj_frame (
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- Incremental frame number within each CUJ.
  frame_number LONG,
  -- vsync id of the frame.
  vsync LONG,
  -- start ts for frame.
  ts TIMESTAMP,
  -- expected start ts for frame.
  ts_expected TIMESTAMP,
  -- start ts of the doFrame associated with frame.
  ts_do_frame_start TIMESTAMP,
  -- The number of GPU completion signals associated with the frame.
  gpu_fence_count LONG,
  -- Whether a draw operation occurred. In cases where there is no 'dirty region', draw is skipped.
  drew_anything LONG,
  -- Whether app frame missed deadline.
  app_missed LONG,
  -- Whether surfaceflinger frame missed deadline.
  sf_missed LONG,
  -- Whether surfaceflinger callback was missed.
  sf_callback_missed LONG,
  -- Whether HWUI callback was missed.
  hwui_callback_missed LONG,
  -- Whether frame finished on time.
  on_time_finish LONG,
  -- actual frame duration.
  dur LONG,
  -- duration based on actual end ts of frame, and doFrame start.
  dur_unadjusted LONG,
  -- expected frame duration.
  dur_expected LONG,
  -- actual end ts of frame.
  ts_end TIMESTAMP
) AS
WITH
  frame_base AS (
    SELECT
      cuj_id,
      row_number() OVER (PARTITION BY cuj_id ORDER BY do_frame.vsync ASC) AS frame_number,
      vsync,
      boundary.ts,
      boundary.ts_expected,
      boundary.ts_do_frame_start,
      count(fence_idx) AS gpu_fence_count,
      count(fence_idx) > 0 AS drew_anything
    FROM _android_jank_cuj_do_frames AS do_frame
    JOIN android_jank_cuj_main_thread_frame_boundary AS boundary
      USING (cuj_id, vsync)
    JOIN android_jank_cuj_draw_frame_slice AS draw_frame
      USING (cuj_id, vsync)
    LEFT JOIN android_jank_cuj_gpu_completion_fence AS fence
      USING (cuj_id, vsync)
    WHERE
      draw_frame.id = fence.draw_frame_slice_id
    GROUP BY
      cuj_id,
      vsync,
      boundary.ts,
      boundary.ts_do_frame_start
  )
SELECT
  frame_base.*,
  app_missed,
  sf_missed,
  sf_callback_missed,
  hwui_callback_missed,
  on_time_finish,
  ts_end_actual - ts AS dur,
  ts_end_actual - ts_do_frame_start AS dur_unadjusted,
  dur_expected,
  ts_end_actual AS ts_end
FROM frame_base
JOIN android_jank_cuj_frame_timeline
  USING (cuj_id, vsync);

-- Similar to `android_jank_cuj_frame` computes overall SF frame boundaries.
-- The computation is somewhat simpler as most of SF work happens within the duration of
-- the commit/composite slices on the main thread.
CREATE PERFETTO TABLE android_jank_cuj_sf_frame (
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- vsync id of the frame.
  vsync LONG,
  -- start ts for frame.
  ts TIMESTAMP,
  -- start ts of expected frame.
  ts_main_thread_start TIMESTAMP,
  -- end ts of sf frame.
  ts_end TIMESTAMP,
  -- actual frame duration.
  dur LONG,
  -- Whether surfaceflinger frame missed deadline.
  sf_missed LONG,
  -- Whether app frame missed deadline.
  app_missed LONG,
  -- Jank tag based on jank type, used for slice visualization.
  jank_tag STRING,
  -- Specify the jank types for this frame if there's jank, or none if no jank occurred.
  jank_type STRING,
  -- Frame's prediction type (eg. valid / expired).
  prediction_type STRING,
  -- Frame's present type (eg. on time / early / late).
  present_type STRING,
  -- Whether the frame used gpu composition.
  gpu_composition LONG,
  -- expected frame duration.
  dur_expected LONG,
  -- Incremental frame number within each CUJ.
  frame_number LONG
) AS
WITH
  android_jank_cuj_timeline_sf_frame AS (
    SELECT DISTINCT
      cuj_id,
      CAST(timeline.name AS INTEGER) AS vsync,
      timeline.display_frame_token
    FROM _android_jank_cuj_vsync_boundary AS boundary
    JOIN actual_frame_timeline_slice AS timeline
      ON boundary.upid = timeline.upid
      AND CAST(timeline.name AS INTEGER) >= vsync_min
      AND CAST(timeline.name AS INTEGER) <= vsync_max
    WHERE
      boundary.layer_id IS NULL
      OR (
        timeline.layer_name GLOB '*#*'
        AND boundary.layer_id = CAST(str_split(timeline.layer_name, '#', 1) AS INTEGER)
      )
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
      -- for simplicity align schema with android_jank_cuj_frame
      NULL AS app_missed,
      jank_tag,
      jank_type,
      prediction_type,
      present_type,
      gpu_composition,
      -- In case expected timeline is missing, as a fallback we use the typical frame deadline
      -- for 60Hz.
      -- See similar expression in android_jank_cuj_frame_timeline.
      coalesce(expected_timeline.dur, 16600000) AS dur_expected
    FROM android_jank_cuj_sf_main_thread_frame_boundary AS boundary, _android_sf_process AS sf_process
    JOIN actual_frame_timeline_slice AS actual_timeline
      ON actual_timeline.upid = sf_process.upid
      AND boundary.vsync = CAST(actual_timeline.name AS INTEGER)
    JOIN android_jank_cuj_timeline_sf_frame AS ft
      ON CAST(actual_timeline.name AS INTEGER) = ft.display_frame_token
      AND boundary.cuj_id = ft.cuj_id
    LEFT JOIN expected_frame_timeline_slice AS expected_timeline
      ON expected_timeline.upid = actual_timeline.upid
      AND expected_timeline.name = actual_timeline.name
  )
SELECT
  *,
  row_number() OVER (PARTITION BY cuj_id ORDER BY vsync ASC) AS frame_number
FROM android_jank_cuj_sf_frame_base;
