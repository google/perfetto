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
SELECT RUN_METRIC('chrome/scroll_jank_cause.sql') AS suppress_query_output;

SELECT
  COUNT(*) AS total,
  SUM(jank) as total_jank,
  SUM(explained_jank + unexplained_jank) AS sum_explained_and_unexplained,
  SUM(
    CASE WHEN explained_jank THEN
      unexplained_jank
    ELSE
      CASE WHEN jank AND NOT unexplained_jank THEN
        1
      ELSE
        0
      END
    END
  ) AS error_rows
FROM scroll_jank_cause;
