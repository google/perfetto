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

-- This is a templated metric that takes 3 parameters:
-- input: name of a table/view which must have columns: id, ts, dur and a
--   "category" column
-- output: name of the view that will be created
-- category: name of the category column in the input table, which will be
--   preserved in the output

SELECT RUN_METRIC('chrome/chrome_processes.sql');
SELECT RUN_METRIC('android/power_drain_in_watts.sql');

DROP TABLE IF EXISTS real_{{input}}_power;
CREATE VIRTUAL TABLE real_{{input}}_power USING SPAN_JOIN(
    {{input}},
    drain_in_watts
);

-- Actual power usage for chrome across the categorised slices contained in the
-- input table broken down by subsystem.
DROP VIEW IF EXISTS {{output}};
CREATE VIEW {{output}} AS
SELECT s.id,
  ts,
  dur,
  {{category}},
  subsystem,
  joules,
  joules / dur * 1e9 AS drain_w
FROM (
    SELECT id,
      subsystem,
      SUM(drain_w * dur / 1e9) AS joules
    FROM real_{{input}}_power
      JOIN power_counters
    WHERE real_{{input}}_power.name = power_counters.name
    GROUP BY id,
      subsystem
  ) p
  JOIN {{input}} s
WHERE s.id = p.id
ORDER BY s.id;
