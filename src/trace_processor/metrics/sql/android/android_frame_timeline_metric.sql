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

SELECT RUN_METRIC('android/process_metadata.sql');

DROP VIEW IF EXISTS android_frame_timeline_metric_per_process;
CREATE VIEW android_frame_timeline_metric_per_process AS
SELECT
  process.upid,
  process.name AS process_name,
  process_metadata.metadata AS process_metadata,
  COUNT(1) AS total_frames,
  SUM(jank_type GLOB '*App Deadline Missed*') AS missed_app_frames,
  SUM(
    jank_type GLOB '*SurfaceFlinger*'
    OR jank_type GLOB '*Prediction Error*'
    OR jank_type GLOB '*Display HAL*') AS missed_sf_frames,
  SUM(jank_type GLOB '*App Deadline Missed*'
    OR jank_type GLOB '*SurfaceFlinger*'
    OR jank_type GLOB '*Prediction Error*'
    OR jank_type GLOB '*Display HAL*') AS missed_frames,
  CAST(PERCENTILE(dur, 50) AS INTEGER) AS frame_dur_p50,
  CAST(PERCENTILE(dur, 90) AS INTEGER) AS frame_dur_p90,
  CAST(PERCENTILE(dur, 95) AS INTEGER) AS frame_dur_p95,
  CAST(PERCENTILE(dur, 99) AS INTEGER) AS frame_dur_p99,
  PERCENTILE(dur / 1e6, 50) AS frame_dur_ms_p50,
  PERCENTILE(dur / 1e6, 90) AS frame_dur_ms_p90,
  PERCENTILE(dur / 1e6, 95) AS frame_dur_ms_p95,
  PERCENTILE(dur / 1e6, 99) AS frame_dur_ms_p99,
  CAST(AVG(dur) AS INTEGER) AS frame_dur_avg,
  MAX(dur) AS frame_dur_max
FROM actual_frame_timeline_slice
JOIN process USING (upid)
JOIN process_metadata USING (upid)
GROUP BY process.upid, process.name;

DROP VIEW IF EXISTS android_frame_timeline_metric_output;
CREATE VIEW android_frame_timeline_metric_output
AS
SELECT
  AndroidFrameTimelineMetric(
    'total_frames', SUM(total_frames),
    'missed_app_frames', SUM(missed_app_frames),
    'process', (
      SELECT
        RepeatedField(
          AndroidFrameTimelineMetric_ProcessBreakdown(
            'process', process_metadata,
            'total_frames', total_frames,
            'missed_frames', missed_frames,
            'missed_app_frames', missed_app_frames,
            'missed_sf_frames', missed_sf_frames,
            'frame_dur_max', frame_dur_max,
            'frame_dur_avg', frame_dur_avg,
            'frame_dur_p50', frame_dur_p50,
            'frame_dur_p90', frame_dur_p90,
            'frame_dur_p95', frame_dur_p95,
            'frame_dur_p99', frame_dur_p99))
      FROM android_frame_timeline_metric_per_process))
FROM android_frame_timeline_metric_per_process;
