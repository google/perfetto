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

-- Charging state intervals per machine, NULL where the state is unknown: every
-- machine with data is filled between its rows, and the primary machine is
-- filled even without data, since the trace says it was there. Other machines
-- without data may not have emitted any, which is not the same as their state
-- being unknown.
CREATE PERFETTO TABLE _android_charging_states_filled AS
FROM (
  WITH
    _counter AS (
      SELECT counter.id, ts, counter.track_id, value
      FROM counter
      JOIN counter_track
        ON counter_track.id = counter.track_id
      WHERE
        counter_track.name = 'BatteryStatus'
    ),
    _intervals AS (
      SELECT
        intervals.id,
        intervals.ts,
        intervals.dur,
        counter_track.machine_id,
        CASE intervals.value
          WHEN 2 THEN 'charging'
          WHEN 3 THEN 'discharging'
          WHEN 4 THEN 'not_charging'
          WHEN 5 THEN 'full'
        END AS short_charging_state,
        CASE intervals.value
          -- 0 and 1 are both 'Unknown'
          WHEN 2 THEN 'Charging'
          WHEN 3 THEN 'Discharging'
          -- special case when charger is present but battery isn't charging
          WHEN 4 THEN 'Not charging'
          WHEN 5 THEN 'Full'
        END AS charging_state
      FROM counter_leading_intervals!(_counter) AS intervals
      JOIN counter_track
        ON counter_track.id = intervals.track_id
      WHERE
        dur > 0
    )
  -- A charging state value which is not a valid enum value is unknown.
  SELECT ts, dur, machine_id, short_charging_state, charging_state
  FROM _intervals
)
|> INTERVAL FILL GAPS WITH (SELECT trace_start() AS ts, trace_dur() AS dur) PER machine_id
|> INTERVAL FILL GAPS WITH (
  SELECT trace_start() AS ts, trace_dur() AS dur, 0 AS machine_id
) PER machine_id;

-- Device charging states.
CREATE PERFETTO TABLE android_charging_states(
  -- A unique id for each row.
  id LONG,
  -- Timestamp at which the device charging state began.
  ts TIMESTAMP,
  -- Duration of the device charging state.
  dur DURATION,
  -- Machine whose charging state is described.
  machine_id JOINID(machine.id),
  -- One of: charging, discharging, not_charging, full, unknown.
  short_charging_state STRING,
  -- Device charging state, one of: Charging, Discharging, Not charging
  -- (when the charger is present but battery is not charging),
  -- Full, Unknown
  charging_state STRING
)
AS
-- Where the charging state is not known, it is 'unknown' for the entirety of
-- the trace.
SELECT
  ROW_NUMBER() OVER (ORDER BY ts, machine_id) AS id,
  ts,
  dur,
  machine_id,
  COALESCE(short_charging_state, 'unknown') AS short_charging_state,
  COALESCE(charging_state, 'Unknown') AS charging_state
FROM _android_charging_states_filled
ORDER BY
  ts,
  machine_id;
