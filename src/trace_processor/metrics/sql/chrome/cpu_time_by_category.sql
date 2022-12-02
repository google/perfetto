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

-- CPU time slices for Chrome threads.
DROP VIEW IF EXISTS chrome_cpu_slices;
CREATE VIEW chrome_cpu_slices AS
SELECT counters.id AS counter_id,
  track_id,
  ts,
  lead(ts) OVER (
    PARTITION BY track_id
    ORDER BY ts
  ) - ts AS dur,
  CAST(
    lead(value) OVER (
      PARTITION BY track_id
      ORDER BY ts
    ) - value AS "INT64"
  ) AS cpu_dur
FROM counters,
  (
    SELECT thread_counter_track.id
    FROM chrome_thread
    JOIN thread_counter_track ON chrome_thread.utid = thread_counter_track.utid
  ) AS t
WHERE t.id = track_id;

DROP TABLE IF EXISTS {{input}}_cpu_time;
CREATE VIRTUAL TABLE {{input}}_cpu_time USING SPAN_JOIN(
  {{input}},
  chrome_cpu_slices PARTITIONED track_id
);

-- View containing the CPU time used (across all cores) for each category slice
-- from input.
-- This will slightly overestimate the CPU time for some category slices as the
-- cpu time slices don't always line up with the category slices. However the
-- CPU slices are small enough this makes very little difference.
DROP VIEW IF EXISTS {{output}};
CREATE VIEW {{output}} AS
SELECT s.id,
  s.ts,
  s.dur,
  s.{{category}},
  SUM(cpu_dur) AS cpu_dur
FROM {{input}}_cpu_time r
JOIN {{input}} s
WHERE r.id = s.id
GROUP BY r.id;
