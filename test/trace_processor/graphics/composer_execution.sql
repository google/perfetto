--
-- Copyright 2021 The Android Open Source Project
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

SELECT RUN_METRIC('android/composer_execution.sql',
  'output', 'hwc_execution_spans') AS suppress_query_output;

SELECT
  validation_type,
  COUNT(*) as count,
  SUM(execution_time_ns) as total
FROM hwc_execution_spans
GROUP BY validation_type
ORDER BY validation_type;
