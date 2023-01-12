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

-- The final table includes the time between the arrival of gesture update
-- input timestamp, and the time it started being processed by CrBrowserMain.

SELECT RUN_METRIC(
  'chrome/chrome_tasks_template.sql',
  'slice_table_name', '{{slice_table_name}}',
  'function_prefix', '{{function_prefix}}'
);

SELECT RUN_METRIC(
  'chrome/chrome_input_to_browser_intervals_base.sql',
  'slice_table_name', '{{slice_table_name}}',
  'function_prefix', '{{function_prefix}}'
);

DROP TABLE IF EXISTS chrome_input_to_browser_intervals;
CREATE TABLE chrome_input_to_browser_intervals
AS
SELECT
  (SELECT ts FROM {{slice_table_name}} WHERE id = window_start_id) AS window_start_ts,
  window_start_id,
  window_end_ts,
  window_end_id,
  blocked_gesture,
  upid,
  {{function_prefix}}GET_SCROLL_TYPE(blocked_gesture, {{function_prefix}}GET_MOJO_PARENT_INTERFACE_TAG(window_end_id)) AS scroll_type
FROM chrome_input_to_browser_interval_slice_ids;
