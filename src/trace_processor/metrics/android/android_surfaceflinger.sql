--
-- Copyright 2020 The Android Open Source Project
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

SELECT RUN_METRIC(
  'android/frame_missed.sql',
  'track_name', 'PrevFrameMissed',
  'output', 'frame_missed'
);
SELECT RUN_METRIC(
  'android/frame_missed.sql',
  'track_name', 'PrevHwcFrameMissed',
  'output', 'hwc_frame_missed'
);
SELECT RUN_METRIC(
  'android/frame_missed.sql',
  'track_name', 'PrevGpuFrameMissed',
  'output', 'gpu_frame_missed'
);

DROP VIEW IF EXISTS android_surfaceflinger_event;
CREATE VIEW android_surfaceflinger_event AS
SELECT
  'slice' AS track_type,
  'Android Missed Frames' AS track_name,
  ts,
  dur,
  'Frame missed' AS slice_name
FROM frame_missed;

DROP VIEW IF EXISTS android_surfaceflinger_output;
CREATE VIEW android_surfaceflinger_output AS
SELECT
  AndroidSurfaceflingerMetric(
    'missed_frames', (SELECT COUNT(1) FROM frame_missed),
    'missed_hwc_frames', (SELECT COUNT(1) FROM hwc_frame_missed),
    'missed_gpu_frames', (SELECT COUNT(1) FROM gpu_frame_missed)
  );
