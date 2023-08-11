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
--
-- WARNING: This metric should not be used as a source of truth. It is under
--          active development and the values & meaning might change without
--          notice.

SELECT RUN_METRIC(
  'chrome/chrome_long_tasks.sql'
);

-- Input tasks/flows are recorded almost the same way as they are in top-level
-- slices, however the input task will not necessarily be a parent of the
-- latencyInfo slice. As such, we can utilize much of the existing input to
-- browser interval base calculations. Determining whether an input is a fling
-- or other blocked input will need to be calculated differently, below.
SELECT RUN_METRIC(
  'chrome/chrome_input_to_browser_intervals_base.sql',
  'slice_table_name', 'slice',
  'function_prefix', ''
);

-- Needed for calculating chrome input to browser intervals.
-- Input IPCs are not necessarily always long tasks, hence a new slice name.
DROP TABLE IF EXISTS chrome_input_to_browser_intervals_long_tasks;
CREATE PERFETTO TABLE chrome_input_to_browser_intervals_long_tasks
AS
SELECT
  (SELECT ts FROM slice WHERE id = window_start_id) AS window_start_ts,
  window_start_id,
  window_end_ts,
  window_end_id,
  blocked_gesture,
  cis.upid,
  GET_SCROLL_TYPE(blocked_gesture, lts.task_name) AS scroll_type
FROM chrome_input_to_browser_interval_slice_ids cis
LEFT JOIN (
  SELECT
    m.task_name,
    m.id,
    upid,
    s.ts,
    s.dur
  FROM
    SELECT_LONG_TASK_SLICES('InterestingTask_ProcessingTime') m
    JOIN slice s USING(id)
    JOIN thread_track tt ON s.track_id = tt.id JOIN thread USING (utid)
) lts
ON cis.upid = lts.upid
  AND (cis.window_end_ts > lts.ts AND cis.window_end_ts <= lts.ts + dur);

-- Calculating chrome tasks delaying input will be the same, just using the
-- long-task based tables instead of chrome_tasks.
SELECT RUN_METRIC(
  'chrome/chrome_tasks_delaying_input_processing_base.sql',
  'duration_causing_jank_ms', '4',
  'task_table_name', 'chrome_long_tasks',
  'input_browser_interval_table_name', 'chrome_input_to_browser_intervals_long_tasks',
  'function_prefix', ''
);