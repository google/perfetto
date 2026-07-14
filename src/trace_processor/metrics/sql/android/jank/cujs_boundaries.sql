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

-- NOTE: We preserve the legacy table names in this file because external
-- consumers and analytical pipelines outside the Perfetto project rely on them.
-- Tables that could be fully migrated to stdlib are re-exported from
-- android.cujs.boundaries. Tables that depend on metrics-only tables
-- (from relevant_slices.sql) remain defined here.

DROP TABLE IF EXISTS android_jank_cuj_vsync_boundary;
CREATE PERFETTO TABLE android_jank_cuj_vsync_boundary AS
SELECT * FROM _android_jank_cuj_vsync_boundary;

-- Similarly, extract the min/max vsync for the SF from
-- commit/compose/onMessageInvalidate slices on its main thread.
DROP TABLE IF EXISTS android_jank_cuj_sf_vsync_boundary;
CREATE PERFETTO TABLE android_jank_cuj_sf_vsync_boundary AS
SELECT
  cuj_id,
  MIN(vsync) AS vsync_min,
  MAX(vsync) AS vsync_max
FROM android_jank_cuj_sf_root_slice
GROUP BY cuj_id;

DROP TABLE IF EXISTS android_jank_cuj_main_thread_frame_boundary;
CREATE PERFETTO TABLE android_jank_cuj_main_thread_frame_boundary AS
SELECT * FROM _android_jank_cuj_main_thread_frame_boundary;

DROP TABLE IF EXISTS android_jank_cuj_main_thread_cuj_boundary;
CREATE PERFETTO TABLE android_jank_cuj_main_thread_cuj_boundary AS
SELECT * FROM _android_jank_cuj_main_thread_cuj_boundary;

DROP TABLE IF EXISTS android_jank_cuj_render_thread_frame_boundary;
CREATE PERFETTO TABLE android_jank_cuj_render_thread_frame_boundary AS
SELECT * FROM _android_jank_cuj_render_thread_frame_boundary;

DROP TABLE IF EXISTS android_jank_cuj_render_thread_cuj_boundary;
CREATE PERFETTO TABLE android_jank_cuj_render_thread_cuj_boundary AS
SELECT * FROM _android_jank_cuj_render_thread_cuj_boundary;

DROP TABLE IF EXISTS android_jank_cuj_boundary;
CREATE PERFETTO TABLE android_jank_cuj_boundary AS
SELECT * FROM _android_jank_cuj_boundary;

-- Similar to `android_jank_cuj_main_thread_frame_boundary`, calculates the frame boundaries
-- based on when we *expected* the work to start and we use the end of the `composite` slice
-- as the end of the work on the frame.
DROP TABLE IF EXISTS android_jank_cuj_sf_main_thread_frame_boundary;
CREATE PERFETTO TABLE android_jank_cuj_sf_main_thread_frame_boundary AS
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
)
SELECT
  cuj_id,
  utid,
  vsync,
  expected_timeline.ts,
  main_thread_slice.ts AS ts_main_thread_start,
  main_thread_slice.ts_end,
  main_thread_slice.ts_end - expected_timeline.ts AS dur
FROM expected_frame_timeline_slice expected_timeline
JOIN android_jank_cuj_sf_process USING (upid)
JOIN main_thread_slice
  ON main_thread_slice.vsync = CAST(expected_timeline.name AS INTEGER);

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
