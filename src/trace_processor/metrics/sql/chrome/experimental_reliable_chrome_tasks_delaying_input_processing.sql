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

-- Script params:
-- {{duration_causing_jank_ms}} : The duration of a single task that would cause
-- jank, by delaying input from being handled on the main thread.

SELECT RUN_METRIC('chrome/chrome_reliable_range.sql') AS suppress_query_output;

DROP VIEW IF EXISTS chrome_reliable_slice;

CREATE PERFETTO VIEW chrome_reliable_slice AS
SELECT *
FROM slice
WHERE ts + dur >= (SELECT start FROM chrome_reliable_range);

SELECT RUN_METRIC(
  'chrome/chrome_tasks_delaying_input_processing_template.sql',
  'duration_causing_jank_ms', '{{duration_causing_jank_ms}}',
  'slice_table_name', 'chrome_reliable_slice',
  'function_prefix', 'EXPERIMENTAL_RELIABLE_'
);
