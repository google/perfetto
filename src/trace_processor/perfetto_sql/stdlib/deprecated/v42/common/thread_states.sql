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

INCLUDE PERFETTO MODULE deprecated.v42.common.timestamps;
INCLUDE PERFETTO MODULE sched.time_in_state;
INCLUDE PERFETTO MODULE sched.states;
INCLUDE PERFETTO MODULE cpu.size;

CREATE PERFETTO FUNCTION _translate_thread_state_name(name STRING)
RETURNS STRING AS
SELECT sched_state_to_human_readable_string($name);


-- Returns a human-readable name for a thread state.
CREATE PERFETTO FUNCTION human_readable_thread_state_name(
  -- Thread state id.
  id INT)
-- Human-readable name for the thread state.
RETURNS STRING AS
SELECT sched_state_io_to_human_readable_string(state, io_wait)
FROM thread_state
WHERE id = $id;

-- Returns an aggregation of thread states (by state and cpu) for a given
-- interval of time for a given thread.
CREATE PERFETTO FUNCTION thread_state_summary_for_interval(
  -- The start of the interval.
  ts INT,
  -- The duration of the interval.
  dur INT,
  -- The utid of the thread.
  utid INT)
RETURNS TABLE(
  -- Human-readable thread state name.
  state STRING,
  -- Raw thread state name, alias of `thread_state.state`.
  raw_state STRING,
  -- The type of CPU if available (e.g. "big" / "mid" / "little").
  cpu_type STRING,
  -- The CPU index.
  cpu INT,
  -- The name of the kernel function execution is blocked in.
  blocked_function STRING,
  -- The total duration.
  dur INT
) AS
SELECT
  sched_state_io_to_human_readable_string(state, io_wait) as state,
  state AS raw_state,
  cpu_guess_core_type(cpu) as cpu_type,
  cpu,
  blocked_function,
  dur
FROM sched_time_in_state_and_cpu_for_thread_in_interval($ts, $dur, $utid);