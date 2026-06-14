--
-- Copyright 2024 The Android Open Source Project
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

INCLUDE PERFETTO MODULE counters.intervals;

-- Counter information for each idle state change for each CPU. Finds each time
-- region where a CPU idle state is constant.
CREATE PERFETTO PIPELINE cpu_idle_counters(
  -- Counter id.
  id LONG,
  -- Joinable with 'counter_track.id'.
  track_id JOINID(track.id),
  -- Starting timestamp of the counter.
  ts TIMESTAMP,
  -- Duration in which the counter is contant and idle state doesn't change.
  dur DURATION,
  -- Idle state of the CPU that corresponds to this counter. An idle state of -1
  -- is defined to be active state for the CPU, and the larger the integer, the
  -- deeper the idle state of the CPU. NULL if not found or undefined.
  idle LONG,
  -- CPU that corresponds to this counter.
  cpu LONG
) AS
SUBPIPELINE idle_events AS (
  FROM counter AS c
  |> JOIN cpu_counter_track AS cct
     ON cct.id = c.track_id AND cct.name = 'cpuidle'
  |> SELECT c.id, c.ts, c.track_id, c.value
)
INTERVALS FROM EVENTS idle_events PER track_id CLOSING LAST AT (trace_end())
|> INTERVAL MERGE CONSECUTIVE BY value AGGREGATE MIN(id) AS id
|> JOIN cpu_counter_track AS cct
   ON track_id = cct.id
|> SELECT
  id,
  track_id,
  ts,
  dur,
  cast_int!(IIF(value = 4294967295, -1, value)) AS idle,
  cct.cpu;
