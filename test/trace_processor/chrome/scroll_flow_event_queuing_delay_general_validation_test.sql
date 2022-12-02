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
SELECT RUN_METRIC('chrome/scroll_flow_event_queuing_delay.sql');

SELECT
  -- Each trace_id (in our example trace not true in general) has 8 steps. There
  -- are 139 scrolls. So we expect 1112 rows in total 72 of which are janky.
  (
    SELECT
      COUNT(*)
    FROM (
      SELECT
        trace_id,
        COUNT(*)
      FROM scroll_flow_event_queuing_delay
      GROUP BY trace_id
    )
  ) AS total_scroll_updates,
  (
    SELECT COUNT(*) FROM scroll_flow_event_queuing_delay
  ) AS total_flow_event_steps,
  (
    SELECT COUNT(*) FROM scroll_flow_event_queuing_delay WHERE jank
  ) AS total_janky_flow_event_steps,
  (
    SELECT COUNT(*) FROM (SELECT step FROM scroll_flow_event_queuing_delay GROUP BY step)
  ) AS number_of_unique_steps;
