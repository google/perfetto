--
-- Copyright 2025 The Android Open Source Project
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
--

-- NOTE (psqlnext): `counter_leading_intervals!` is `INTERVALS FROM CHANGES … PER
-- track_id CLOSING LAST AT(trace_end())` plus `INTERVAL MERGE CONSECUTIVE BY
-- value` to collapse equal-valued runs.

-- GPU power state which is analogous to CPU idle state
CREATE PERFETTO PIPELINE android_mali_gpu_power_state(
  -- Timestamp
  ts TIMESTAMP,
  -- Duration
  dur DURATION,
  -- GPU power state
  power_state LONG
) MATERIALIZED AS
SUBPIPELINE power_state_events AS (
  FROM counter AS c
  |> JOIN counter_track AS t ON t.id = c.track_id AND t.name = 'mali_gpu_power_state'
  |> SELECT c.id, c.ts, c.track_id, c.value
)
INTERVALS FROM CHANGES power_state_events PER track_id CLOSING LAST AT (trace_end())
|> INTERVAL MERGE CONSECUTIVE BY value
|> SELECT ts, dur, cast_int!(value) AS power_state;
