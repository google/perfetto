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

INCLUDE PERFETTO MODULE android.cujs.boundaries;

-- Stores the min and max vsync IDs for each of the CUJs which are extracted
-- from the CUJ markers. For backward compatibility (In case the markers don't
-- exist), We calculate that by extracting the vsync ID from the
-- `Choreographer#doFrame` slices that are within the CUJ markers.
DROP TABLE IF EXISTS android_jank_cuj_vsync_boundary;
CREATE PERFETTO TABLE android_jank_cuj_vsync_boundary AS
SELECT
  *
FROM _android_jank_cuj_vsync_boundary;

-- Similar to `android_jank_cuj_main_thread_frame_boundary` but for the render
-- thread the expected start time is the time of the first `postAndWait` slice
-- on the main thread.
-- The query is slightly simpler because we don't have to handle the clock drift
-- and data loss like with the frame timeline.
-- One difference vs main thread is that there might be multiple DrawFrames
-- slices for a single vsync - this happens when we are drawing multiple layers
-- (e.g. status bar & notifications).
DROP TABLE IF EXISTS android_jank_cuj_render_thread_frame_boundary;
CREATE PERFETTO TABLE android_jank_cuj_render_thread_frame_boundary AS
-- see do_frame_ordered above
-- we also order by `ts` to handle multiple DrawFrames for a single vsync
WITH draw_frame_ordered AS (
  SELECT
    *,
    -- ts_end of the previous draw frame, or -1 if no previous draw frame found
    COALESCE(LAG(ts_end) OVER (PARTITION BY cuj_id ORDER BY vsync ASC, ts ASC), -1) AS ts_prev_draw_frame_end
  FROM android_jank_cuj_draw_frame_slice
),
-- introducing an intermediate table since we want to calculate dur = ts_end - ts
frame_boundary_base AS (
  SELECT
    draw_frame.cuj_id,
    draw_frame.utid,
    draw_frame.vsync,
    MIN(post_and_wait.ts) AS ts_expected,
    MIN(draw_frame.ts) AS ts_draw_frame_start,
    MIN(draw_frame.ts_prev_draw_frame_end) AS ts_prev_draw_frame_end,
    MIN(
      MAX(
        MIN(post_and_wait.ts),
        MIN(draw_frame.ts_prev_draw_frame_end)),
      MIN(draw_frame.ts)) AS ts,
    MAX(draw_frame.ts_end) AS ts_end
  FROM draw_frame_ordered draw_frame
  JOIN _android_jank_cuj_do_frames do_frame USING (cuj_id, vsync)
  JOIN descendant_slice(do_frame.id) post_and_wait
  WHERE post_and_wait.name = 'postAndWait'
  GROUP BY draw_frame.cuj_id, draw_frame.utid, draw_frame.vsync
)
SELECT
  *,
  ts_end - ts AS dur
FROM frame_boundary_base;

-- Compute the CUJ boundary on the render thread from the frame boundaries.
DROP TABLE IF EXISTS android_jank_cuj_render_thread_cuj_boundary;
CREATE PERFETTO TABLE android_jank_cuj_render_thread_cuj_boundary AS
SELECT
  cuj_id,
  utid,
  MIN(ts) AS ts,
  MAX(ts_end) AS ts_end,
  MAX(ts_end) - MIN(ts) AS dur
FROM android_jank_cuj_render_thread_frame_boundary
GROUP BY cuj_id, utid;

-- Compute the CUJ boundary on the main thread from the frame boundaries.
DROP TABLE IF EXISTS android_jank_cuj_sf_main_thread_cuj_boundary;
CREATE PERFETTO TABLE android_jank_cuj_sf_main_thread_cuj_boundary AS
SELECT
  cuj_id,
  utid,
  MIN(ts) AS ts,
  MAX(ts_end) AS ts_end,
  MAX(ts_end) - MIN(ts) AS dur
FROM android_jank_cuj_sf_main_thread_frame_boundary
GROUP BY cuj_id, utid;

-- RenderEngine will only work on a frame if SF falls back to client composition.
-- Because of that we do not calculate overall CUJ boundaries so there is no
-- `android_jank_cuj_sf_render_engine_cuj_boundary` table created.
-- RenderEngine frame boundaries are calculated based on `composeSurfaces` slice start
-- on the main thread (this calls into RenderEngine), and when `REThreaded::drawLayers`
-- ends.
DROP TABLE IF EXISTS android_jank_cuj_sf_render_engine_frame_boundary;
CREATE PERFETTO TABLE android_jank_cuj_sf_render_engine_frame_boundary AS
SELECT
  cuj_id,
  utid,
  vsync,
  draw_layers.ts_compose_surfaces AS ts,
  draw_layers.ts AS ts_draw_layers_start,
  draw_layers.ts_end,
  draw_layers.ts_end - draw_layers.ts_compose_surfaces AS dur
FROM android_jank_cuj_sf_draw_layers_slice draw_layers;

DROP TABLE IF EXISTS android_jank_cuj_sf_boundary;
CREATE PERFETTO TABLE android_jank_cuj_sf_boundary AS
SELECT cuj_id, ts, ts_end, dur
FROM android_jank_cuj_sf_main_thread_cuj_boundary;
