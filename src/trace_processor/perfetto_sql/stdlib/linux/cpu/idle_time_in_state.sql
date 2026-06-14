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

-- Percentage counter information for sysfs cpuidle states.
-- For each state per cpu, report the incremental time spent in one state,
-- divided by time spent in all states, between two timestamps.
CREATE PERFETTO PIPELINE linux_per_cpu_idle_time_in_state_counters(
  -- Timestamp.
  ts TIMESTAMP,
  -- The machine this residency is calculated for.
  machine_id JOINID(machine.id),
  -- State name.
  state STRING,
  -- CPU.
  cpu LONG,
  -- Percentage of time this cpu spent in this state.
  idle_percentage DOUBLE,
  -- Incremental time spent in this state (residency), in microseconds.
  total_residency DOUBLE,
  -- Time this cpu spent in any state, in microseconds.
  time_slice LONG
)
MATERIALIZED AS
SUBPIPELINE idle_states AS (
  FROM counter AS c
  |> JOIN track AS t ON c.track_id = t.id
  |> WHERE t.type = 'cpu_idle_state'
  |> SELECT
    c.ts,
    c.value,
    c.track_id,
    t.machine_id,
    extract_arg(t.dimension_arg_set_id, 'state') AS state,
    extract_arg(t.dimension_arg_set_id, 'cpu') AS cpu
)
FROM idle_states
|> SELECT
  ts,
  state,
  cpu,
  idle_states.machine_id,
  track_id,
  value - (lag(value) OVER (PARTITION BY track_id ORDER BY ts)) AS total_residency,
  (time_to_us(ts - lag(ts, 1) OVER (PARTITION BY track_id ORDER BY ts))) AS time_slice
|> FORK AS residency_deltas
|> WHERE time_slice IS NOT NULL
|> SELECT
  ts,
  machine_id,
  state,
  cpu,
  min(100, (total_residency / time_slice) * 100) AS idle_percentage,
  total_residency,
  time_slice
|> UNION ALL (
  -- Calculate c0 state by subtracting all other states from total time.
  FROM residency_deltas
  |> WHERE time_slice IS NOT NULL
  |> AGGREGATE
    'C0' AS state,
    max(0, ((ANY_VALUE(time_slice) - sum(total_residency)) / ANY_VALUE(time_slice)) * 100) AS idle_percentage,
    max(0, ANY_VALUE(time_slice) - sum(total_residency)) AS total_residency,
    ANY_VALUE(time_slice) AS time_slice
    GROUP BY ts, cpu, machine_id
  |> SELECT ts, machine_id, state, cpu, idle_percentage, total_residency, time_slice
);

-- Percentage counter information for sysfs cpuidle states.
-- For each state across all CPUs, report the incremental time spent in one
-- state, divided by time spent in all states, between two timestamps.
CREATE PERFETTO PIPELINE linux_cpu_idle_time_in_state_counters(
  -- Timestamp.
  ts TIMESTAMP,
  -- The machine this residency is calculated for.
  machine_id JOINID(machine.id),
  -- State name.
  state STRING,
  -- Percentage of time all CPUS spent in this state.
  idle_percentage DOUBLE,
  -- Incremental time spent in this state (residency), in microseconds.
  total_residency DOUBLE,
  -- Time all CPUS spent in any state, in microseconds.
  time_slice LONG
)
MATERIALIZED AS
FROM linux_per_cpu_idle_time_in_state_counters
|> AGGREGATE
  min(100, (sum(total_residency) / sum(time_slice)) * 100) AS idle_percentage,
  sum(total_residency) AS total_residency,
  sum(time_slice) AS time_slice
  GROUP BY ts, state, machine_id;
