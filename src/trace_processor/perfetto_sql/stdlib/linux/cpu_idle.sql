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

-- CPU frequency counter per core.
CREATE VIEW internal_cpu_freq_counters
AS
SELECT
  ts,
  dur,
  value AS freq_value,
  cct.cpu
FROM experimental_counter_dur ecd
LEFT JOIN cpu_counter_track cct
  ON ecd.track_id = cct.id
WHERE cct.name = 'cpufreq';

-- CPU idle counter per core.
CREATE VIEW internal_cpu_idle_counters
AS
SELECT
  ts,
  dur,
  -- Correct 4294967295 to -1 (both of them means an exit from the current state).
  iif(value = 4294967295, -1, CAST(value AS int)) AS idle_value,
  cct.cpu
FROM experimental_counter_dur ecd
LEFT JOIN cpu_counter_track cct
  ON ecd.track_id = cct.id
WHERE cct.name = 'cpuidle';

-- Combined cpu freq & idle counter
CREATE VIRTUAL TABLE internal_freq_idle_counters
USING
  span_join(internal_cpu_freq_counters PARTITIONED cpu, internal_cpu_idle_counters PARTITIONED cpu);

-- Aggregates cpu idle statistics per core.
--
-- @column cpu             CPU core number.
-- @column state           CPU idle state (C-states).
-- @column count           The count of entering idle state.
-- @column dur             Total CPU core idle state duration in nanoseconds.
-- @column avg_dur         Average CPU core idle state duration in nanoseconds.
-- @column idle_percent    Idle state percentage of non suspend time (C-states + P-states).
CREATE PERFETTO TABLE linux_cpu_idle_stats
AS
WITH
total AS (
  SELECT
    cpu,
    sum(dur) AS dur
  FROM internal_freq_idle_counters
  GROUP BY cpu
)
SELECT
  cpu,
  (idle_value + 1) AS state,
  COUNT(idle_value) AS count,
  SUM(dur) AS dur,
  SUM(dur) / COUNT(idle_value) AS avg_dur,
  SUM(dur) * 100.0 / (SELECT dur FROM total t WHERE t.cpu = ific.cpu) AS idle_percent
FROM internal_freq_idle_counters ific
WHERE idle_value >=0
GROUP BY cpu, idle_value;

