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
-- change: one H.264/HEVC access unit (plus a one-off codec_config row per
-- stream, is_config = 1).
--
-- The encoded payload is held zero-copy in trace_processor (a TraceBlobView
-- over the original trace blob). Fetch the bytes with
-- __INTRINSIC_VIDEO_FRAME_AU_DATA(id) which returns a BLOB.
CREATE PERFETTO TABLE android_video_frames(
  -- Row id.
  id ID(__intrinsic_video_frames.id),
  -- Timestamp of the frame capture (boot-time instant the screen changed).
  ts TIMESTAMP,
  -- Per-session display index (0, 1, ...) assigned by the producer.
  display_id LONG,
  -- Human-readable display name (from the codec_config packet).
  display_name STRING,
  -- RFC 6381 codec string (from the codec_config packet),
  -- e.g. "avc1.42c00b" / "hev1.1.6.L93.B0".
  codec_string STRING,
  -- Sequential frame number within the capture session.
  frame_number LONG,
  -- Codec (1 = H264, 2 = HEVC).
  codec LONG,
  -- 1 if the access unit is a key frame (IDR).
  is_key_frame LONG,
  -- Codec presentation timestamp of the access unit, microseconds.
  pts_us LONG,
  -- 1 if this row carries codec_config (decoder setup), not a frame.
  is_config LONG
)
AS
SELECT
  id,
  ts,
  display_id,
  display_name,
  codec_string,
  frame_number,
  codec,
  is_key_frame,
  pts_us,
  is_config
FROM __intrinsic_video_frames;

-- Per-stream failure indicators from VideoFrameError packets are surfaced
-- as kIndexed entries in the global `stats` table, keyed by display_id
-- (one entry per VideoFrameError.Reason; same shape as ftrace's per-cpu
-- stats):
--   * android_video_size_cap_hit           (DisplayVideoConfig.
--                                           max_stream_size_bytes reached)
--   * android_video_codec_error            (MediaCodec onError)
--   * android_video_display_gone           (source display removed)
--   * android_video_no_encoder             (no h/w encoder on device)
--   * android_video_display_not_found      (display gone at session start)
--   * android_video_encoder_setup_failed   (MediaCodec configure threw)
--   * android_video_virtual_display_failed (createVirtualDisplay null)
-- Clean streams produce no rows. Example:
--   SELECT name, idx AS display_id, value FROM stats
--   WHERE name LIKE 'android_video_%';

