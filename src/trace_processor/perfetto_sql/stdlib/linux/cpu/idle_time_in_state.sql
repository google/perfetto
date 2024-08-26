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
INCLUDE PERFETTO MODULE time.conversion;

-- Counter information for sysfs cpuidle states.
-- Tracks the percentage of time spent in each state between two timestamps, by
-- dividing the incremental time spent in one state, by time all CPUS spent in
-- any state.
CREATE PERFETTO TABLE cpu_idle_time_in_state_counters(
  -- Timestamp.
  ts LONG,
  -- State name.
  state_name STRING,
  -- Percentage of time all CPUS spent in this state.
  idle_percentage DOUBLE,
  -- Incremental time spent in this state (residency), in microseconds.
  total_residency DOUBLE,
  -- Time all CPUS spent in any state, in microseconds.
  time_slice INT
) AS
WITH residency_deltas AS (
  SELECT
  ts,
  c.name as state_name,
  value - (LAG(value) OVER (PARTITION BY c.name, cct.cpu ORDER BY ts)) as delta
  FROM counters c
  JOIN cpu_counter_track cct on c.track_id=cct.id
  WHERE c.name GLOB 'cpuidle.*'
),
total_residency_calc AS (
SELECT
  ts,
  state_name,
  sum(delta) as total_residency,
  -- Perfetto timestamp is in nanoseconds whereas sysfs cpuidle time
  -- is in microseconds.
  (
    (SELECT count(distinct cpu) from cpu_counter_track) *
    (time_to_us(ts - LAG(ts,1) over (partition by state_name order by ts)))
  )  as time_slice
  FROM residency_deltas
GROUP BY ts, state_name
)
SELECT
  ts,
  state_name,
  MIN(100, (total_residency / time_slice) * 100) as idle_percentage,
  total_residency,
  time_slice
FROM total_residency_calc
WHERE time_slice IS NOT NULL
UNION ALL
-- Calculate c0 state by subtracting all other states from total time.
SELECT
  ts,
  'cpuidle.C0' as state_name,
  (MAX(0,time_slice - SUM(total_residency)) / time_slice) * 100 AS idle_percentage,
  time_slice - SUM(total_residency),
  time_slice
FROM total_residency_calc
WHERE time_slice IS NOT NULL
GROUP BY ts;
