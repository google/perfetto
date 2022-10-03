--
-- Copyright 2021 The Android Open Source Project
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

SELECT RUN_METRIC('chrome/touch_flow_event_queuing_delay.sql')
 ;

-- trace 6911 is janky and 6915 and 6940 succeed it (both are not janky).
SELECT
  trace_id,
  jank,
  step,
  next_step,
  ancestor_end,
  maybe_next_ancestor_ts,
  queuing_time_ns
FROM touch_flow_event_queuing_delay
WHERE trace_id = 6915 OR trace_id = 6911 OR trace_id = 6940
ORDER BY trace_id, ts;
