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

-- Counter information for each frequency change for each CPU. Finds each time
-- region where a CPU frequency is constant.
CREATE PERFETTO PIPELINE cpu_frequency_counters(
  -- Counter id.
  id LONG,
  -- Joinable with 'counter_track.id'.
  track_id JOINID(track.id),
  -- Starting timestamp of the counter
  ts TIMESTAMP,
  -- Duration in which counter is constant and frequency doesn't change.
  dur DURATION,
  -- Frequency in kHz of the CPU that corresponds to this counter. NULL if not
  -- found or undefined.
  freq LONG,
  -- Unique CPU id.
  ucpu LONG,
  -- CPU that corresponds to this counter.
  cpu LONG
)
MATERIALIZED AS
SUBPIPELINE cpufreq_events AS (
  FROM counter AS c
  |> JOIN cpu_counter_track AS cct
    ON cct.id = c.track_id AND cct.name = 'cpufreq'
  |> SELECT c.id, c.ts, c.track_id, c.value
)
INTERVALS FROM CHANGES cpufreq_events PER track_id CLOSING LAST AT (trace_end())
|> INTERVAL MERGE CONSECUTIVE BY value
   AGGREGATE MIN(id) AS id, MIN(track_id) AS track_id
|> JOIN cpu_counter_track AS cct ON track_id = cct.id
|> JOIN cpu
  ON cct.machine_id IS cpu.machine_id
  AND cct.cpu = cpu.cpu
|> SELECT
  id,
  track_id,
  ts,
  dur,
  cast_int!(value) AS freq,
  cpu.ucpu,
  cct.cpu;
