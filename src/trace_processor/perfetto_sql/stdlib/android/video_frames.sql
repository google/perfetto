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

-- Video frames captured from the device display. Each row is one screen
-- change: a v1 still (JPEG/WEBP) or, for v2 hardware video, one H.264/HEVC
-- access unit (plus a one-off codec_config row per stream, is_config = 1).
CREATE PERFETTO TABLE android_video_frames(
  -- Row id. Pass to video_frame_image() to get the payload bytes (still image,
  -- access unit, or codec config depending on the row).
  id ID(__intrinsic_video_frames.id),
  -- Timestamp of the frame capture (boot-time instant the screen changed).
  ts TIMESTAMP,
  -- Sequential frame number within the capture session.
  frame_number LONG,
  -- Reference to the track table (from TrackEvent.track_uuid).
  track_id JOINID(track.id),
  -- Display name of the track (from TrackDescriptor).
  track_name STRING,
  -- v2 codec (1 = H264, 2 = HEVC); NULL for v1 still images.
  codec LONG,
  -- v2: 1 if the access unit is a key frame (IDR).
  is_key_frame LONG,
  -- v2: codec presentation timestamp of the access unit, microseconds.
  pts_us LONG,
  -- v2: 1 if this row carries codec_config (decoder setup), not a frame.
  is_config LONG
)
AS
SELECT
  vf.id,
  vf.ts,
  vf.frame_number,
  vf.track_id,
  t.name AS track_name,
  vf.codec,
  vf.is_key_frame,
  vf.pts_us,
  vf.is_config
FROM __intrinsic_video_frames AS vf
JOIN track AS t
  ON vf.track_id = t.id;
