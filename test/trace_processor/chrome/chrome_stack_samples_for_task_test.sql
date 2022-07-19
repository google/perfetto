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

SELECT RUN_METRIC('chrome/chrome_stack_samples_for_task.sql',
    'target_duration_ms', '0.000001',
    'thread_name', '"CrBrowserMain"',
    'task_name', '"sendTouchEvent"') AS suppress_query_output;

SELECT
    sample.description,
    sample.ts,
    sample.depth
FROM chrome_stack_samples_for_task sample
JOIN (
    SELECT
        ts,
        dur
    FROM slice
    WHERE ts = 696373965001470
) test_slice
    ON sample.ts >= test_slice.ts
    AND sample.ts <= test_slice.ts + test_slice.dur;