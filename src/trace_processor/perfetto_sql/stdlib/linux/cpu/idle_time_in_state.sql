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
  ts TIMESTAMP,
  -- The machine this residency is calculated for.
  machine_id LONG,
  -- State name.
  state_name STRING,
  -- Percentage of time all CPUS spent in this state.
  idle_percentage DOUBLE,
  -- Incremental time spent in this state (residency), in microseconds.
  total_residency DOUBLE,
  -- Time all CPUS spent in any state, in microseconds.
  time_slice LONG
) AS
WITH cpu_counts_per_machine AS (
  SELECT machine_id, count(1) AS cpu_count
  FROM cpu
  GROUP BY machine_id
),
idle_states AS (
  SELECT
    c.ts,
    c.value,
    c.track_id,
    t.machine_id,
    EXTRACT_ARG(t.dimension_arg_set_id, 'cpu_idle_state') as state,
    EXTRACT_ARG(t.dimension_arg_set_id, 'cpu') as cpu
  FROM counter c
  JOIN track t on c.track_id = t.id
  WHERE t.type = 'cpu_idle_state'
),
residency_deltas AS (
  SELECT
    ts,
    state,
    cpu,
    machine_id,
    value - (LAG(value) OVER (PARTITION BY track_id ORDER BY ts)) as delta
  FROM idle_states
),
total_residency_calc AS (
  SELECT
    ts,
    residency_deltas.machine_id,
    state AS state_name,
    SUM(delta) as total_residency,
    -- Perfetto timestamp is in nanoseconds whereas sysfs cpuidle time
    -- is in microseconds.
    (
      cpu_counts_per_machine.cpu_count *
      (time_to_us(ts - LAG(ts, 1) over (PARTITION BY state ORDER BY ts)))
    )  as time_slice
  FROM residency_deltas
  -- The use of `IS` instead of `=` is intentional because machine_id can be
  -- null and we still want this join to work in that case.
  JOIN cpu_counts_per_machine
    ON residency_deltas.machine_id IS cpu_counts_per_machine.machine_id
  GROUP BY ts, residency_deltas.machine_id, state
)
SELECT
  ts,
  machine_id,
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
  machine_id,
  'C0' as state_name,
  (MAX(0,time_slice - SUM(total_residency)) / time_slice) * 100 AS idle_percentage,
  time_slice - SUM(total_residency),
  time_slice
FROM total_residency_calc
WHERE time_slice IS NOT NULL
GROUP BY ts;
