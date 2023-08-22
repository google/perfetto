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


CREATE PERFETTO TABLE internal_cpu_sizes AS
SELECT 0 AS n, 'little' AS size
UNION
SELECT 1 AS n, 'mid' AS size
UNION
SELECT 2 AS n, 'big' AS size;

CREATE PERFETTO TABLE internal_ranked_cpus AS
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
--
-- @arg cpu_index INT   Index of the CPU whose size we will guess.
-- @ret STRING          A descriptive size ('little', 'mid', 'big', etc) or NULL if we have insufficient information.
CREATE PERFETTO FUNCTION guess_cpu_size(cpu_index INT)
RETURNS STRING AS
SELECT
  IIF((SELECT COUNT(DISTINCT n) FROM internal_ranked_cpus) >= 2, size, null) as size
FROM internal_ranked_cpus
LEFT JOIN internal_cpu_sizes USING(n)
WHERE cpu = $cpu_index;
