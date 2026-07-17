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

INCLUDE PERFETTO MODULE intervals.intersect;

DROP VIEW IF EXISTS android_jank_cuj_slice;
CREATE PERFETTO VIEW android_jank_cuj_slice AS
SELECT
  cuj_id,
  process.upid,
  process.name AS process_name,
  thread.utid,
  thread.name AS thread_name,
  slice.*,
  slice.ts + slice.dur AS ts_end
FROM android_jank_cuj_boundary boundary
JOIN process USING (upid)
JOIN thread USING (upid)
JOIN thread_track USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
    -- Take slices which overlap even they started before the boundaries
    -- This is to be able to query slices that delayed start of a frame
    AND slice.ts + slice.dur >= boundary.ts AND slice.ts <= boundary.ts_end
WHERE slice.dur > 0;

DROP TABLE IF EXISTS android_jank_cuj_main_thread_slice;
CREATE PERFETTO TABLE android_jank_cuj_main_thread_slice AS
SELECT
  cuj_id,
  upid,
  utid,
  slice.*,
  slice.ts + slice.dur AS ts_end
FROM android_jank_cuj_main_thread_cuj_boundary boundary
JOIN thread_track USING (utid)
JOIN thread USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
    -- Take slices which overlap even they started before the boundaries
    -- This is to be able to query slices that delayed start of a frame
    AND slice.ts + slice.dur >= boundary.ts
    AND slice.ts <= boundary.ts_end
WHERE slice.dur > 0;

DROP TABLE IF EXISTS android_jank_cuj_render_thread_slice;
CREATE PERFETTO TABLE android_jank_cuj_render_thread_slice AS
SELECT
  cuj_id,
  upid,
  utid,
  slice.*,
  slice.ts + slice.dur AS ts_end
FROM android_jank_cuj_render_thread_cuj_boundary boundary
JOIN thread_track USING (utid)
JOIN thread USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
    -- Take slices which overlap even they started before the boundaries
    -- This is to be able to query slices that delayed start of a frame
    AND slice.ts + slice.dur >= boundary.ts
    AND slice.ts <= boundary.ts_end
WHERE slice.dur > 0;

DROP VIEW IF EXISTS android_jank_cuj_sf_slice;
CREATE PERFETTO VIEW android_jank_cuj_sf_slice AS
SELECT
  cuj_id,
  upid,
  sf_process.name AS process_name,
  thread.utid,
  thread.name AS thread_name,
  slice.*,
  slice.ts + slice.dur AS ts_end
FROM android_jank_cuj_sf_boundary sf_boundary
JOIN android_jank_cuj_sf_process sf_process
JOIN thread USING (upid)
JOIN thread_track USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
    -- Take slices which overlap even they started before the boundaries
    -- This is to be able to query slices that delayed start of a frame
    AND slice.ts + slice.dur >= sf_boundary.ts AND slice.ts <= sf_boundary.ts_end
WHERE slice.dur > 0;

-- The SF main thread is very busy, so even though there are only a handful of
-- (wide) per-CUJ boundaries, the naive "boundary JOIN slice ON <overlap>" nested
-- loop is expensive. As with the RenderEngine table above, we use
-- `interval_intersect` (interval tree, O(n log n)) with the two inputs
-- materialized into their own tables.
DROP TABLE IF EXISTS android_jank_cuj_sf_main_thread_boundary_intervals;
CREATE PERFETTO TABLE android_jank_cuj_sf_main_thread_boundary_intervals AS
SELECT
  -- interval_intersect requires an integer `id` on each input.
  ROW_NUMBER() OVER (ORDER BY boundary.ts) AS id,
  boundary.ts,
  boundary.ts_end - boundary.ts AS dur,
  boundary.utid,
  boundary.cuj_id,
  thread.upid
FROM android_jank_cuj_sf_main_thread_cuj_boundary boundary
JOIN thread USING (utid);

DROP TABLE IF EXISTS android_jank_cuj_sf_main_thread_track_slice;
CREATE PERFETTO TABLE android_jank_cuj_sf_main_thread_track_slice AS
SELECT
  slice.id,
  slice.ts,
  slice.dur,
  thread_track.utid
FROM slice
JOIN thread_track
  ON slice.track_id = thread_track.id
WHERE
  thread_track.utid IN (
    SELECT DISTINCT utid FROM android_jank_cuj_sf_main_thread_boundary_intervals
  )
  AND slice.dur > 0;

DROP TABLE IF EXISTS android_jank_cuj_sf_main_thread_slice;
CREATE PERFETTO TABLE android_jank_cuj_sf_main_thread_slice AS
SELECT
  boundary.cuj_id,
  boundary.upid,
  boundary.utid,
  slice.*,
  slice.ts + slice.dur AS ts_end
FROM _interval_intersect!(
  (
    android_jank_cuj_sf_main_thread_boundary_intervals,
    android_jank_cuj_sf_main_thread_track_slice
  ),
  (utid)
) ii
JOIN android_jank_cuj_sf_main_thread_boundary_intervals boundary
  ON boundary.id = ii.id_0
JOIN slice
  ON slice.id = ii.id_1;

-- For RenderEngine thread we use a different approach as it's only used when SF falls back to
-- client composition. Instead of taking all slices during CUJ, we look at each frame explicitly
-- and only take slices that are within RenderEngine frame boundaries.
--
-- Unlike the other slice tables above (which have few, wide, per-CUJ boundaries),
-- RenderEngine has one narrow boundary per frame - hundreds of them - so the naive
-- "boundary JOIN slice ON <overlap>" nested loop scans the (very busy) RenderEngine
-- track once per boundary and is by far the most expensive query in this metric. We
-- instead use `interval_intersect`, which matches boundaries to overlapping slices in
-- O(n log n) via an interval tree. The two inputs are materialized into their own
-- tables (rather than CTEs) because the macro references each input several times and
-- would otherwise re-evaluate them.
DROP TABLE IF EXISTS android_jank_cuj_sf_render_engine_boundary_intervals;
CREATE PERFETTO TABLE android_jank_cuj_sf_render_engine_boundary_intervals AS
SELECT
  -- interval_intersect requires an integer `id` on each input.
  ROW_NUMBER() OVER (ORDER BY boundary.ts) AS id,
  boundary.ts,
  boundary.dur,
  boundary.utid,
  boundary.cuj_id,
  thread.upid
FROM android_jank_cuj_sf_render_engine_frame_boundary boundary
JOIN thread USING (utid);

DROP TABLE IF EXISTS android_jank_cuj_sf_render_engine_track_slice;
CREATE PERFETTO TABLE android_jank_cuj_sf_render_engine_track_slice AS
SELECT
  slice.id,
  slice.ts,
  slice.dur,
  thread_track.utid
FROM slice
JOIN thread_track
  ON slice.track_id = thread_track.id
WHERE
  thread_track.utid IN (
    SELECT DISTINCT utid FROM android_jank_cuj_sf_render_engine_boundary_intervals
  )
  AND slice.dur > 0;

DROP TABLE IF EXISTS android_jank_cuj_sf_render_engine_slice;
CREATE PERFETTO TABLE android_jank_cuj_sf_render_engine_slice AS
SELECT
  boundary.cuj_id,
  boundary.upid,
  boundary.utid,
  slice.*,
  slice.ts + slice.dur AS ts_end
FROM _interval_intersect!(
  (
    android_jank_cuj_sf_render_engine_boundary_intervals,
    android_jank_cuj_sf_render_engine_track_slice
  ),
  (utid)
) ii
JOIN android_jank_cuj_sf_render_engine_boundary_intervals boundary
  ON boundary.id = ii.id_0
JOIN slice
  ON slice.id = ii.id_1;
