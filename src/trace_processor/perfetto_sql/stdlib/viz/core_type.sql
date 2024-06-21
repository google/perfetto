--
-- Copyright 2023 The Android Open Source Project
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

-- This module contains all the things depending on a very broken
-- implementation of guessing core type. This module only exists to
-- avoid breaking features in the UI: it's very strongly recommended
-- that we replace all the functionality in this module with something
-- more accurate.

INCLUDE PERFETTO MODULE intervals.overlap;

CREATE PERFETTO TABLE _cpu_sizes AS
SELECT 0 AS n, 'little' AS size
UNION
SELECT 1 AS n, 'mid' AS size
UNION
SELECT 2 AS n, 'big' AS size;

CREATE PERFETTO TABLE _ranked_cpus AS
SELECT
 (DENSE_RANK() OVER win) - 1 AS n,
 cpu
FROM (
  SELECT
    track.cpu AS cpu,
    MAX(counter.value) AS maxfreq
  FROM counter
  JOIN cpu_counter_track AS track
  ON (counter.track_id = track.id)
  WHERE track.name = "cpufreq"
  GROUP BY track.cpu
)
WINDOW win AS (ORDER BY maxfreq);

-- Guess size of CPU.
-- On some multicore devices the cores are heterogeneous and divided
-- into two or more 'sizes'. In a typical case a device might have 8
-- cores of which 4 are 'little' (low power & low performance) and 4
-- are 'big' (high power & high performance). This functions attempts
-- to map a given CPU index onto the relevant descriptor. For
-- homogeneous systems this returns NULL.
CREATE PERFETTO FUNCTION _guess_core_type(
  -- Index of the CPU whose size we will guess.
  cpu_index INT)
-- A descriptive size ('little', 'mid', 'big', etc) or NULL if we have insufficient information.
RETURNS STRING AS
SELECT
  IIF((SELECT COUNT(DISTINCT n) FROM _ranked_cpus) >= 2, size, null) as size
FROM _ranked_cpus
LEFT JOIN _cpu_sizes USING(n)
WHERE cpu = $cpu_index;

-- All of the CPUs with their core type as a descriptive size ('little', 'mid', 'big', etc).
CREATE PERFETTO TABLE _guessed_core_types(
  -- Index of the CPU.
  cpu_index INT,
  -- A descriptive size ('little', 'mid', 'big', etc) or NULL if we have insufficient information.
  size STRING
) AS
SELECT
  cpu as cpu_index,
  _guess_core_type(cpu) AS size
FROM _ranked_cpus;

-- The count of active CPUs with a given core type over time.
CREATE PERFETTO FUNCTION _active_cpu_count_for_core_type(
  -- Type of the CPU core as reported by GUESS_CPU_SIZE. Usually 'big', 'mid' or 'little'.
  core_type STRING
) RETURNS TABLE(
  -- Timestamp when the number of active CPU changed.
  ts LONG,
  -- Number of active CPUs, covering the range from this timestamp to the next
  -- row's timestamp.
  active_cpu_count LONG
) AS
WITH
-- Materialise the relevant cores to avoid calling a function for each row of the sched table.
cores AS MATERIALIZED (
  SELECT cpu_index
  FROM _guessed_core_types
  WHERE size = $core_type
),
-- Filter sched events corresponding to running tasks.
-- utid=0 is the swapper thread / idle task.
tasks AS (
  SELECT ts, dur
  FROM sched
  WHERE
    cpu IN (SELECT cpu_index FROM cores)
    AND utid != 0
)
SELECT
  ts, value as active_cpu_count
FROM intervals_overlap_count!(tasks, ts, dur)
ORDER BY ts;
