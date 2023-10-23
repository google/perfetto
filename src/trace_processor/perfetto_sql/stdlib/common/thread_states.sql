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

INCLUDE PERFETTO MODULE common.timestamps;
INCLUDE PERFETTO MODULE common.cpus;

-- TODO(altimin): this doesn't handle some corner cases which thread_state.ts
-- handles (as complex strings manipulations in SQL are pretty painful),
-- but they are pretty niche.
-- Translates the thread state name from a single-letter shorthard to
-- a human-readable name.
CREATE PERFETTO FUNCTION internal_translate_thread_state_name(name STRING)
RETURNS STRING AS
SELECT CASE $name
WHEN 'Running' THEN 'Running'
WHEN 'R' THEN 'Runnable'
WHEN 'R+' THEN 'Runnable (Preempted)'
WHEN 'S' THEN 'Sleeping'
WHEN 'D' THEN 'Uninterruptible Sleep'
WHEN 'T' THEN 'Stopped'
WHEN 't' THEN 'Traced'
WHEN 'X' THEN 'Exit (Dead)'
WHEN 'Z' THEN 'Exit (Zombie)'
WHEN 'x' THEN 'Task Dead'
WHEN 'I' THEN 'Idle'
WHEN 'K' THEN 'Wakekill'
WHEN 'W' THEN 'Waking'
WHEN 'P' THEN 'Parked'
WHEN 'N' THEN 'No Load'
ELSE $name
END;

-- Returns a human-readable name for a thread state.
-- @arg id INT  Thread state id.
-- @ret STRING  Human-readable name for the thread state.
CREATE PERFETTO FUNCTION human_readable_thread_state_name(id INT)
RETURNS STRING AS
WITH data AS (
  SELECT
    internal_translate_thread_state_name(state) AS state,
    (CASE io_wait
      WHEN 1 THEN ' (IO)'
      WHEN 0 THEN ' (non-IO)'
      ELSE ''
    END) AS io_wait
  FROM thread_state
  WHERE id = $id
)
SELECT
  printf('%s%s', state, io_wait)
FROM data;

-- Returns an aggregation of thread states (by state and cpu) for a given
-- interval of time for a given thread.
-- @arg ts INT       The start of the interval.
-- @arg dur INT      The duration of the interval.
-- @arg utid INT     The utid of the thread.
-- @column state     Human-readable thread state name.
-- @column raw_state Raw thread state name, alias of `thread_state.state`.
-- @column cpu_type  The type of CPU if available (e.g. "big" / "mid" / "little").
-- @column cpu       The CPU index.
-- @column blocked_function The name of the kernel function execution is blocked in.
-- @column dur       The total duration.
CREATE PERFETTO FUNCTION thread_state_summary_for_interval(
  ts INT, dur INT, utid INT)
RETURNS TABLE(
  state STRING, raw_state STRING, cpu_type STRING, cpu INT, blocked_function STRING, dur INT)
AS
WITH
states_starting_inside AS (
  SELECT id
  FROM thread_state
  WHERE $ts <= ts
    AND ts <= $ts + $dur
    AND utid = $utid
),
first_state_starting_before AS (
  SELECT id
  FROM thread_state
  WHERE ts < $ts AND utid = $utid
  ORDER BY ts DESC
  LIMIT 1
),
relevant_states AS (
  SELECT * FROM states_starting_inside
  UNION ALL
  SELECT * FROM first_state_starting_before
)
SELECT
  human_readable_thread_state_name(id) as state,
  state as raw_state,
  guess_cpu_size(cpu) as cpu_type,
  cpu,
  blocked_function,
  sum(spans_overlapping_dur($ts, $dur, ts, dur)) as dur
FROM thread_state
JOIN relevant_states USING (id)
GROUP BY state, raw_state, cpu_type, cpu, blocked_function
ORDER BY dur desc;