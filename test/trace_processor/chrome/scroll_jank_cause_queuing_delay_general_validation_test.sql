--
-- Copyright 2020 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
SELECT RUN_METRIC('chrome/scroll_jank_cause_queuing_delay.sql')
    AS suppress_query_output;

SELECT
  COUNT(*) as total,
  (
    SELECT
      DISTINCT(avg_no_jank_dur_overlapping_ns)
    FROM scroll_jank_cause_queuing_delay
    WHERE
      location = "LatencyInfo.Flow" AND
      jank
  ) AS janky_latency_info_non_jank_avg_dur,
  (
    SELECT
      DISTINCT(avg_no_jank_dur_overlapping_ns)
    FROM scroll_jank_cause_queuing_delay
    WHERE
      location = "LatencyInfo.Flow" AND
      NOT jank
  ) AS non_janky_latency_info_non_jank_avg_dur
FROM (
  SELECT
    trace_id
  FROM scroll_jank_cause_queuing_delay
  GROUP BY trace_id
);
