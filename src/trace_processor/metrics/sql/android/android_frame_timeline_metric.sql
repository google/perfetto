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

DROP VIEW IF EXISTS app_deadline_missed_view;

CREATE VIEW app_deadline_missed_view
AS
SELECT p.name AS process, COUNT(jank_type) AS 'jank_count'
FROM actual_frame_timeline_slice
LEFT JOIN process AS p
  USING (upid)
WHERE jank_type LIKE '%App Deadline Missed%'
GROUP BY process;

DROP VIEW IF EXISTS android_frame_timeline_metric_output;

CREATE VIEW android_frame_timeline_metric_output
AS
SELECT
    AndroidFrameTimelineMetric(
    'app_deadline_missed_total_count',
    (SELECT SUM(jank_count) FROM app_deadline_missed_view),
    'process',
    (
      SELECT
        RepeatedField(
            AndroidFrameTimelineMetric_ProcessBreakdown(
            'process_name',
            process,
            'app_deadline_missed_count',
            jank_count))
      FROM app_deadline_missed_view
    ));
