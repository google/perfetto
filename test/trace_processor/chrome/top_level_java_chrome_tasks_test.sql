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
  'chrome/chrome_tasks_template.sql',
  'slice_table_name', 'slice',
  'function_prefix', ''
);

-- Verify that toplevel,Java events are recorded correctly - that Choreographer
-- tasks are recorded (task_type = "choreographer"); that non-embedded Java
-- events don't include toplevel Java events (task_type != "java"); and that
-- choreographer events include descendent frames.
SELECT
  full_name,
  task_type
FROM chrome_tasks
WHERE category = "toplevel,Java"
AND ts < 263904000000000
GROUP BY full_name, task_type;