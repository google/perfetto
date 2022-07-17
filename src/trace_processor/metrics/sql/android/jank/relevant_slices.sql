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

SELECT CREATE_FUNCTION(
  'VSYNC_FROM_NAME(slice_name STRING)',
  'STRING',
  'SELECT CAST(STR_SPLIT($slice_name, " ", 1) AS INTEGER)'
);

SELECT CREATE_FUNCTION(
  'GPU_COMPLETION_FENCE_ID_FROM_NAME(slice_name STRING)',
  'STRING',
  'SELECT
    CASE
      WHEN
        $slice_name GLOB "GPU completion fence *"
      THEN
        CAST(STR_SPLIT($slice_name, " ", 3) AS INTEGER)
      WHEN
        $slice_name GLOB "Trace GPU completion fence *"
      THEN
        CAST(STR_SPLIT($slice_name, " ", 4) AS INTEGER)
      WHEN
        $slice_name GLOB "waiting for GPU completion *"
      THEN
        CAST(STR_SPLIT($slice_name, " ", 4) AS INTEGER)
      WHEN
        $slice_name GLOB "waiting for HWC release *"
      THEN
        CAST(STR_SPLIT($slice_name, " ", 4) AS INTEGER)
      ELSE NULL
    END
  '
);

-- Find Choreographer#doFrame slices that are between the CUJ markers.
-- We extract vsync IDs from doFrame slice names and use these as the source
-- of truth that allow us to get correct slices on the other threads.
DROP TABLE IF EXISTS android_jank_cuj_do_frame_slice;
CREATE TABLE android_jank_cuj_do_frame_slice AS
SELECT
  cuj.cuj_id,
  main_thread.utid,
  slice.*,
  slice.ts + slice.dur AS ts_end,
  VSYNC_FROM_NAME(slice.name) AS vsync
FROM android_jank_cuj cuj
JOIN slice
  ON slice.ts + slice.dur >= cuj.ts AND slice.ts <= cuj.ts_end
JOIN android_jank_cuj_main_thread main_thread
  ON cuj.cuj_id = main_thread.cuj_id
  AND main_thread.track_id = slice.track_id
WHERE
  slice.name GLOB 'Choreographer#doFrame*'
  AND slice.dur > 0;


-- Store render thread DrawFrames by matching in the vsync IDs extracted from
-- doFrame slices. In case of multiple layers being drawn, there might be
-- multiple DrawFrames for a single vsync.
DROP TABLE IF EXISTS android_jank_cuj_draw_frame_slice;
CREATE TABLE android_jank_cuj_draw_frame_slice AS
SELECT
  cuj_id,
  render_thread.utid,
  slice.*,
  slice.ts + slice.dur AS ts_end,
  VSYNC_FROM_NAME(slice.name) AS vsync
FROM android_jank_cuj_do_frame_slice do_frame
JOIN android_jank_cuj_render_thread render_thread USING (cuj_id)
JOIN slice
  ON slice.track_id = render_thread.track_id
WHERE slice.name GLOB 'DrawFrame*'
AND VSYNC_FROM_NAME(slice.name) = do_frame.vsync
AND slice.dur > 0;

-- Find descendants of DrawFrames which contain the GPU completion fence ID that
-- is used for signaling that the GPU finished drawing.
DROP TABLE IF EXISTS android_jank_cuj_gpu_completion_fence;
CREATE TABLE android_jank_cuj_gpu_completion_fence AS
SELECT
  cuj_id,
  vsync,
  draw_frame.id AS draw_frame_slice_id,
  GPU_COMPLETION_FENCE_ID_FROM_NAME(fence.name) AS fence_idx
FROM android_jank_cuj_draw_frame_slice draw_frame
JOIN descendant_slice(draw_frame.id) fence
  ON fence.name GLOB '*GPU completion fence*';

-- Find GPU completion slices which indicate when the GPU finished drawing.
DROP TABLE IF EXISTS android_jank_cuj_gpu_completion_slice;
CREATE TABLE android_jank_cuj_gpu_completion_slice AS
SELECT
  fence.cuj_id,
  vsync,
  slice.*,
  slice.ts + slice.dur AS ts_end,
  fence.fence_idx
FROM android_jank_cuj_gpu_completion_thread gpu_completion_thread
JOIN slice USING (track_id)
JOIN android_jank_cuj_gpu_completion_fence fence
  ON fence.cuj_id = gpu_completion_thread.cuj_id
  AND fence.fence_idx = GPU_COMPLETION_FENCE_ID_FROM_NAME(slice.name)
WHERE
  slice.name GLOB 'waiting for GPU completion *'
  AND slice.dur > 0;

-- Find HWC release slices which indicate when the HWC released the buffer.
DROP TABLE IF EXISTS android_jank_cuj_hwc_release_slice;
CREATE TABLE android_jank_cuj_hwc_release_slice AS
SELECT
  fence.cuj_id,
  vsync,
  slice.*,
  slice.ts + slice.dur AS ts_end,
  fence.fence_idx
FROM android_jank_cuj_hwc_release_thread hwc_release_thread
JOIN slice USING (track_id)
JOIN android_jank_cuj_gpu_completion_fence fence
  ON fence.cuj_id = hwc_release_thread.cuj_id
  AND fence.fence_idx = GPU_COMPLETION_FENCE_ID_FROM_NAME(slice.name)
WHERE
  slice.name GLOB 'waiting for HWC release *'
  AND slice.dur > 0;
