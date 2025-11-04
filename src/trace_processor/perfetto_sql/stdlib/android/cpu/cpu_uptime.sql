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

INCLUDE PERFETTO MODULE linux.cpu.idle;

CREATE PERFETTO TABLE _sched_processes_for_span AS
SELECT
  ss.ts,
  ss.dur,
  ss.cpu,
  ss.id,
  t.tid,
  t.name AS thread_name,
  p.pid,
  p.name AS process_name
FROM sched_slice AS ss
JOIN thread AS t
  USING (utid)
JOIN process AS p
  USING (upid)
WHERE
  p.name IS NOT NULL;

CREATE PERFETTO TABLE _cpu_active_for_span AS
WITH
  step1 AS (
    SELECT
      ts,
      dur,
      cpu,
      ts AS group_ts,
      dur AS group_dur,
      lead(ts) OVER (PARTITION BY cpu ORDER BY ts) AS next_group_ts
    FROM cpu_idle_counters
    WHERE
      idle = -1
  )
SELECT
  *,
  row_number() OVER (ORDER BY ts) AS group_id
FROM step1;

CREATE VIRTUAL TABLE _android_active_sched_joined USING SPAN_JOIN (_cpu_active_for_span PARTITIONED cpu, _sched_processes_for_span PARTITIONED cpu);

-- Table which groups scheduling information along with the CPU idle state
-- information. This is meant to be used in conjunciton with the android_cpu_uptime_cost
-- calculation macro.
CREATE PERFETTO TABLE android_active_sched_joined (
  -- timestamp
  ts LONG,
  -- duration
  dur LONG,
  -- cpu
  cpu LONG,
  -- id,
  id LONG,
  -- unique group id per each active period in the CPU
  group_id LONG,
  -- timestamp of the start of the active CPU usage
  group_ts LONG,
  -- duration of active CPU usage
  group_dur LONG,
  -- timestamp for the next active period in group
  next_group_ts LONG,
  -- thread ID
  tid LONG,
  -- thread name
  thread_name STRING,
  -- process id
  pid LONG,
  -- process name
  process_name STRING
) AS
SELECT
  *
FROM _android_active_sched_joined;

-- Macro: android_cpu_uptime_cost
--
-- This macro calculates the theoretical CPU uptime cost attributed to each thread/process.
-- It works by assigning the "cost" (idle duration required) to enter deeper C-states (C2, C3, etc.)
-- to the thread that kept the CPU active during that time, preventing the C-state entry.
--
-- Prerequisite Table: Use this macro in conjunction with the android_cpu_sched_joined
-- table, which provides thread scheduling and active/idle durations.
--
-- Arguments: The costs to enter C2 and C3 states (expressed as required idle duration in microseconds).
--
-- Usage Example:
-- -- Assuming C2 entry cost is 1000 microseconds and C3 entry cost is 10000 microseconds:
--
--     android_cpu_uptime_cost!(
--         android_active_sched_joined,  -- Input table
--         1000,                         -- C2 entry cost (microseconds)
--         10000                         -- C3 entry cost (microseconds)
--     )
--
-- This generates a table of the CPU uptime cost grouped by each thread/process.
--
-- Underlying Calculation Logic:
-- The cost (C2_cost + C3_cost) is assigned to the first thread that causes the CPU to become
-- active and exits the idle state. Subsequent threads in the active duration are only
-- assigned their own scheduled duration.
--
-- Given:
-- * Active_dur: Duration where the CPU is continuously active.
-- * Sched1_dur: Scheduled duration of the first process after the CPU becomes active.
-- * SchedN_dur: Scheduled duration of the Nth process in the active window.
-- * C2_cost: Idle duration needed to enter C2 state.
-- * C3_cost: Idle duration needed to enter C3 state.
--
-- Calculation:
-- * Uptime1 (First Process): Sched1_dur + C2_cost + C3_cost
-- * UptimeN (Subsequent Processes, N > 1): SchedN_dur
CREATE PERFETTO MACRO android_cpu_uptime_cost(
    -- Generated table from android_active_sched_joined
    active_sched_joined TableOrSubquery,
    -- cost to enter the C2 state in microseconds
    c2_cost Expr,
    -- cost to enter the C3 state in microseconds
    c3_cost Expr
)
RETURNS TableOrSubquery AS
(
  WITH
    step1 AS (
      SELECT
        *,
        -- Determine within the span of active durations, which is the first
        -- process to perform work
        CASE
          WHEN row_number() OVER (PARTITION BY group_id ORDER BY ts ASC) = 1
          THEN TRUE
          ELSE FALSE
        END AS is_first_work
      FROM $active_sched_joined
    ),
    step2 AS (
      SELECT
        *,
        next_group_ts,
        next_group_ts - (
          group_ts + group_dur
        ) AS diff,
        -- Assign the cost of going into C2 to the first thread in the active duration
        CASE
          WHEN is_first_work = TRUE
          THEN min($c2_cost * 1000, next_group_ts - (
            group_ts + group_dur
          ))
          ELSE 0
        END AS first_work_cost_c2,
        -- Assign the cost of going into C3 into the first thread in active duration
        CASE
          WHEN is_first_work = TRUE
          THEN min($c3_cost * 1000, next_group_ts - (
            group_ts + group_dur
          ))
          ELSE 0
        END AS first_work_cost_c3,
        dur AS work_cost
      FROM step1
    ),
    step3 AS (
      SELECT
        *,
        dur + first_work_cost_c2 AS uptime_cost_c2,
        dur + first_work_cost_c3 AS uptime_cost_c3
      FROM step2
    )
  SELECT
    sum(uptime_cost_c2) AS uptime_cost_c2,
    sum(uptime_cost_c3) AS uptime_cost_c3,
    sum(dur) AS execution_time,
    thread_name,
    process_name
  FROM step3
  GROUP BY
    thread_name,
    process_name
  ORDER BY
    uptime_cost_c2 DESC
);
