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

SELECT RUN_METRIC('chrome/scroll_flow_event_queuing_delay.sql')
  AS suppress_query_output;

-- trace 2725 is janky and 2721 and 2727 surround it (both are not janky). We
-- just manually computed these values to ensure the queuing time is correct.
SELECT
  trace_id,
  step,
  next_step,
  ancestor_end,
  maybe_next_ancestor_ts,
  queuing_time_ns
FROM scroll_flow_event_queuing_delay
WHERE trace_id = 2721 OR trace_id = 2725 OR trace_id = 2727
ORDER BY trace_id, ts;
