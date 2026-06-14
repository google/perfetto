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

-- NOTE (psqlnext): the original used a DISTINCT-ts spine LEFT JOINed against
-- one subquery per `batt.*` series. This is a wide pivot (long-form series ->
-- one column per value), which §10 leaves to §4 conditional aggregation: the
-- pivot below groups the `batt.*` counters by ts and folds each series into its
-- own column with MAX(CASE ...), equivalent to the per-name LEFT JOINs.

-- Battery charge at timestamp.
CREATE PERFETTO PIPELINE android_battery_charge(
  -- Timestamp.
  ts TIMESTAMP,
  -- Current average micro ampers.
  current_avg_ua DOUBLE,
  -- Current capacity percentage.
  capacity_percent DOUBLE,
  -- Current charge in micro ampers.
  charge_uah DOUBLE,
  -- Current micro ampers.
  current_ua DOUBLE,
  -- Current voltage in micro volts.
  voltage_uv DOUBLE,
  -- Current energy counter in microwatt-hours(µWh).
  energy_counter_uwh DOUBLE,
  -- Current power in milliwatts.
  power_mw DOUBLE
)
AS
FROM counter AS c
|> JOIN counter_track AS t ON c.track_id = t.id
|> WHERE t.name GLOB 'batt.*'
|> AGGREGATE
     max(CASE WHEN t.name = 'batt.current.avg_ua' THEN c.value END) AS current_avg_ua,
     max(CASE WHEN t.name = 'batt.capacity_pct' THEN c.value END) AS capacity_percent,
     max(CASE WHEN t.name = 'batt.charge_uah' THEN c.value END) AS charge_uah,
     max(CASE WHEN t.name = 'batt.current_ua' THEN c.value END) AS current_ua,
     max(CASE WHEN t.name = 'batt.voltage_uv' THEN c.value END) AS voltage_uv,
     max(CASE WHEN t.name = 'batt.energy_counter_uwh' THEN c.value END) AS energy_counter_uwh,
     max(CASE WHEN t.name = 'batt.power_mw' THEN c.value END) AS power_mw
   GROUP BY c.ts
|> ORDER BY ts;
