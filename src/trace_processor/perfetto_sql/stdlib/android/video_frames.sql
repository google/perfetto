--
-- Copyright 2026 The Android Open Source Project
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

-- Displayable frames of the display video (from the android.display.video
-- data source), one row per encoded frame. Codec config rows are excluded.
-- The encoded bytes of a frame can be fetched with
-- `__intrinsic_video_frame_au_data(id)`.
CREATE PERFETTO TABLE android_video_frames(
  -- Id of the frame, the same as the id in `__intrinsic_video_frames`.
  id ID,
  -- Time the frame was presented (its presentation timestamp, in trace time).
  ts TIMESTAMP,
  -- Time until the next frame of the same display, i.e. how long this frame
  -- was on screen. NULL for the last frame of a display.
  dur DURATION,
  -- Logical id of the source display (the `DisplayManager` id), the same id
  -- space as `android_motion_events.display_id`.
  display_id LONG,
  -- Human-readable name of the source display.
  display_name STRING,
  -- Sequential frame number within the capture session.
  frame_number LONG,
  -- Whether this frame is a key frame (decodable on its own).
  is_key_frame BOOL,
  -- Frame timeline vsync id of the SurfaceFlinger composite that produced
  -- this frame (`actual_frame_timeline_slice.display_frame_token`). NULL if
  -- the frame could not be linked.
  frame_timeline_vsync_id LONG,
  -- Logical width, in pixels, of the source display when the frame was
  -- captured. The picture shows this area scaled to fit, preserving aspect
  -- ratio and centred. NULL for traces that don't record it.
  source_width LONG,
  -- Logical height, in pixels, of the source display when the frame was
  -- captured. NULL for traces that don't record it.
  source_height LONG
)
AS
SELECT
  id,
  ts,
  lead(ts) OVER (PARTITION BY display_id ORDER BY ts, id) - ts AS dur,
  display_id,
  display_name,
  frame_number,
  is_key_frame,
  frame_timeline_vsync_id,
  source_width,
  source_height
FROM __intrinsic_video_frames
WHERE
  coalesce(is_config, 0) = 0
ORDER BY
  id;
