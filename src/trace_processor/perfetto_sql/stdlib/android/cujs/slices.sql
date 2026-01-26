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

INCLUDE PERFETTO MODULE android.cujs.threads;

-- Extract vsync id from slice name.
CREATE PERFETTO FUNCTION _vsync_from_name(
    slice_name STRING
)
RETURNS STRING AS
SELECT
  CAST(str_split($slice_name, " ", 1) AS INTEGER);

-- Extract gpu completion fence id from the provided slice name.
CREATE PERFETTO FUNCTION _gpu_completion_fence_id_from_name(
    -- slice name.
    slice_name STRING
)
RETURNS STRING AS
SELECT
  CASE
    WHEN $slice_name GLOB "GPU completion fence *"
    THEN CAST(str_split($slice_name, " ", 3) AS INTEGER)
    WHEN $slice_name GLOB "Trace GPU completion fence *"
    THEN CAST(str_split($slice_name, " ", 4) AS INTEGER)
    WHEN $slice_name GLOB "waiting for GPU completion *"
    THEN CAST(str_split($slice_name, " ", 4) AS INTEGER)
    WHEN $slice_name GLOB "Trace HWC release fence *"
    THEN CAST(str_split($slice_name, " ", 4) AS INTEGER)
    WHEN $slice_name GLOB "waiting for HWC release *"
    THEN CAST(str_split($slice_name, " ", 4) AS INTEGER)
    ELSE NULL
  END;

-- Store render thread DrawFrames by matching in the vsync IDs extracted from
-- doFrame slices. In case of multiple layers being drawn, there might be
-- multiple DrawFrames for a single vsync.
CREATE PERFETTO TABLE android_jank_cuj_draw_frame_slice (
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- process id.
  upid JOINID(process.id),
  -- thread id of the input thread.
  utid JOINID(thread.id),
  -- id of the slice.
  id JOINID(slice.id),
  -- start timestamp of the slice.
  ts TIMESTAMP,
  -- end timestamp of the draw frame slice.
  ts_end TIMESTAMP,
  -- vsync id of the frame.
  vsync LONG
) AS
SELECT
  cuj_id,
  render_thread.upid,
  render_thread.utid,
  slice.id,
  slice.ts,
  slice.ts + slice.dur AS ts_end,
  _vsync_from_name(slice.name) AS vsync
FROM _android_jank_cuj_do_frames AS do_frame
JOIN android_jank_cuj_render_thread AS render_thread
  USING (cuj_id)
JOIN slice
  ON slice.track_id = render_thread.track_id
WHERE
  slice.name GLOB 'DrawFrame*'
  AND _vsync_from_name(slice.name) = do_frame.vsync
  AND slice.dur > 0;

-- Find descendants of DrawFrames which contain the GPU completion fence ID that
-- is used for signaling that the GPU finished drawing.
CREATE PERFETTO TABLE android_jank_cuj_gpu_completion_fence (
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- vsync id of the frame.
  vsync LONG,
  -- slice if of the draw frame.
  draw_frame_slice_id LONG,
  -- gpu completion fence id for draw frame.
  fence_idx LONG
) AS
SELECT
  cuj_id,
  vsync,
  draw_frame.id AS draw_frame_slice_id,
  _gpu_completion_fence_id_from_name(fence.name) AS fence_idx
FROM android_jank_cuj_draw_frame_slice AS draw_frame
JOIN descendant_slice(draw_frame.id) AS fence
  ON fence.name GLOB '*GPU completion fence*';
