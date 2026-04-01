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

-- Video frames captured from the device display.
CREATE PERFETTO TABLE android_video_frames (
  -- Row id. Pass to video_frame_image() to get the JPEG bytes.
  id ID(__intrinsic_video_frames.id),
  -- Timestamp of the frame capture.
  ts TIMESTAMP,
  -- Sequential frame number within the capture session.
  frame_number LONG,
  -- Reference to the track table (from TrackEvent.track_uuid).
  track_id JOINID(track.id),
  -- Display name of the track (from TrackDescriptor).
  track_name STRING
) AS
SELECT
  vf.id,
  vf.ts,
  vf.frame_number,
  vf.track_id,
  t.name AS track_name
FROM __intrinsic_video_frames AS vf
JOIN track AS t
  ON vf.track_id = t.id;
