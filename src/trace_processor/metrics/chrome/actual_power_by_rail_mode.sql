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
--

SELECT RUN_METRIC('chrome/rail_modes.sql');
SELECT RUN_METRIC('chrome/chrome_processes.sql');
SELECT RUN_METRIC('android/power_drain_in_watts.sql');

DROP TABLE IF EXISTS real_rail_power;
CREATE VIRTUAL TABLE real_rail_power USING SPAN_JOIN(
    combined_overall_rail_slices,
    drain_in_watts
);

-- Actual power usage for chrome across the RAIL mode slices contained in
-- combined_overall_rail_slices broken down by subsystem.
DROP VIEW IF EXISTS real_power_by_rail_mode;
CREATE VIEW real_power_by_rail_mode AS
SELECT s.id,
  ts,
  dur,
  rail_mode,
  subsystem,
  joules,
  joules / dur * 1e9 AS drain_w
FROM (
    SELECT id,
      subsystem,
      SUM(drain_w * dur / 1e9) AS joules
    FROM real_rail_power
      JOIN power_counters
    WHERE real_rail_power.name = power_counters.name
    GROUP BY id,
      subsystem
  ) p
  JOIN combined_overall_rail_slices s
WHERE s.id = p.id
ORDER BY s.id;
