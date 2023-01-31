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

-- This test runs the on a trace that includes both LongTask tracking traces
-- and top-level traces. This test verifies that the default input processing
-- delay metric can be calculated while both scenarios are enabled. The output
-- should be consistent (same tasks) as the test for the LongTask version of the
-- metric - chrome_long_tasks_delaying_input_processing_test.sql

SELECT RUN_METRIC(
  'chrome/chrome_tasks_delaying_input_processing.sql',
  'duration_causing_jank_ms', '4'
);

SELECT
  full_name,
  duration_ms,
  slice_id
FROM chrome_tasks_delaying_input_processing
ORDER BY slice_id;