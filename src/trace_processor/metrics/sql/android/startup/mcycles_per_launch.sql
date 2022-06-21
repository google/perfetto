--
-- Copyright 2022 The Android Open Source Project
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

-- Create the base CPU span join table.
SELECT RUN_METRIC('android/android_cpu_agg.sql');
SELECT RUN_METRIC('android/cpu_info.sql');

-- Create a span join safe launches view; since both views
-- being span joined have an "id" column, we need to rename
-- the id column for launches to disambiguate the two.
DROP VIEW IF EXISTS launches_span_join_safe;
CREATE VIEW launches_span_join_safe AS
SELECT ts, dur, id AS launch_id
FROM launches;

-- Span join the CPU table with the launches table to get the
-- breakdown per-cpu.
DROP TABLE IF EXISTS cpu_freq_sched_per_thread_per_launch;
CREATE VIRTUAL TABLE cpu_freq_sched_per_thread_per_launch
USING SPAN_JOIN(
  launches_span_join_safe,
  cpu_freq_sched_per_thread PARTITIONED cpu
);

-- Materialized to avoid span-joining once per core type.
DROP TABLE IF EXISTS mcycles_per_core_type_per_launch;
CREATE TABLE mcycles_per_core_type_per_launch AS
SELECT
  launch_id,
  IFNULL(core_type_per_cpu.core_type, 'unknown') AS core_type,
  CAST(SUM(dur * freq_khz / 1000) / 1e9 AS INT) AS mcycles
FROM cpu_freq_sched_per_thread_per_launch
LEFT JOIN core_type_per_cpu USING (cpu)
WHERE utid != 0
GROUP BY 1, 2;

-- Given a launch id and core type, returns the number of mcycles consumed
-- on CPUs of that core type during the launch.
SELECT CREATE_FUNCTION(
  'MCYCLES_FOR_LAUNCH_AND_CORE_TYPE(launch_id INT, core_type STRING)',
  'INT', "
    SELECT mcycles
    FROM mcycles_per_core_type_per_launch m
    WHERE m.launch_id = $launch_id AND m.core_type = $core_type
  "
);
