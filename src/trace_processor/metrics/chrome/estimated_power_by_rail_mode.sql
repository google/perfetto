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
SELECT RUN_METRIC('android/android_proxy_power.sql');

-- View containing estimated power slices broken down by cpu.
DROP VIEW IF EXISTS power_per_chrome_thread;
CREATE VIEW power_per_chrome_thread AS
SELECT ts,
  dur,
  cpu,
  power_per_thread.utid,
  end_state,
  priority,
  power_ma,
  power_per_thread.type,
  name AS thread_name,
  upid,
  is_main_thread
FROM power_per_thread
  JOIN chrome_thread
WHERE power_per_thread.utid = chrome_thread.utid;

DROP TABLE IF EXISTS rail_power;
CREATE VIRTUAL TABLE rail_power USING SPAN_JOIN(
  combined_overall_rail_slices,
  power_per_chrome_thread
);

-- Estimated power usage for chrome across the RAIL mode slices contained in
-- combined_overall_rail_slices.
DROP VIEW IF EXISTS power_by_rail_mode;
CREATE VIEW power_by_rail_mode AS
SELECT id,
  ts,
  dur,
  rail_mode,
  mas,
  mas / dur * 1e9 AS ma
FROM (
    SELECT s.id,
      s.ts,
      s.dur,
      s.rail_mode,
      SUM(r.power_ma * r.dur) / 1e9 AS mas
    FROM rail_power r
      JOIN combined_overall_rail_slices s
    WHERE r.id == s.id
    GROUP BY s.id
  );
