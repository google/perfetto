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

-- NOTE (psqlnext): the `counters.intervals` and `intervals.fill_gaps` modules are
-- DELETED — `counter_leading_intervals!` is `INTERVALS FROM EVENTS` (+`MERGE
-- CONSECUTIVE BY value`), and `_intervals_fill_gaps!` is `INTERVAL FILL … WITHIN`.

-- Device charging states.
CREATE PERFETTO PIPELINE android_charging_states(
  -- A unique id for each row.
  id LONG,
  -- Timestamp at which the device charging state began.
  ts TIMESTAMP,
  -- Duration of the device charging state.
  dur DURATION,
  -- One of: charging, discharging, not_charging, full, unknown.
  short_charging_state STRING,
  -- Device charging state, one of: Charging, Discharging, Not charging
  -- (when the charger is present but battery is not charging),
  -- Full, Unknown
  charging_state STRING
)
MATERIALIZED AS
SUBPIPELINE battery_status_events AS (
  FROM counter
  |> JOIN counter_track ON counter_track.id = counter.track_id
  |> WHERE counter_track.name = 'BatteryStatus'
  |> SELECT counter.id, counter.ts, 0 AS track_id, counter.value
)
-- Counter samples become intervals of constant state; equal-valued runs coalesce.
INTERVALS FROM EVENTS battery_status_events PER track_id CLOSING LAST AT (trace_end())
|> INTERVAL MERGE CONSECUTIVE BY value
|> WHERE dur > 0
|> SELECT
     ts,
     dur,
     CASE value
       WHEN 2 THEN 'charging'
       WHEN 3 THEN 'discharging'
       WHEN 4 THEN 'not_charging'
       WHEN 5 THEN 'full'
     END AS short_charging_state,
     CASE value
       -- 0 and 1 are both 'Unknown'
       WHEN 2 THEN 'Charging'
       WHEN 3 THEN 'Discharging'
       -- special case when charger is present but battery isn't charging
       WHEN 4 THEN 'Not charging'
       WHEN 5 THEN 'Full'
     END AS charging_state
-- Cover the whole trace: spans with no slice (or an invalid enum value) become
-- filler rows with null payload, defaulted to 'unknown' below.
|> INTERVAL FILL WITHIN trace_bounds
|> SELECT
     ROW_NUMBER() OVER () AS id,
     ts,
     dur,
     COALESCE(short_charging_state, 'unknown') AS short_charging_state,
     COALESCE(charging_state, 'Unknown') AS charging_state;
