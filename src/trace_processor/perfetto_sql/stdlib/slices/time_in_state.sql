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

INCLUDE PERFETTO MODULE slices.with_context;

-- For each thread slice, returns the sum of the time it spent in various
-- scheduling states.
--
-- Requires scheduling data to be available in the trace.
CREATE PERFETTO PIPELINE thread_slice_time_in_state(
  -- Thread slice.
  id JOINID(slice.id),
  -- Name of the slice.
  name STRING,
  -- Thread the slice is running on.
  utid JOINID(thread.id),
  -- Name of the thread.
  thread_name STRING,
  -- Id of the process the slice is running on.
  upid JOINID(process.id),
  -- Name of the process.
  process_name STRING,
  -- The scheduling state (from the `thread_state` table).
  --
  -- Use the `sched_state_to_human_readable_string` function in the `sched`
  -- package to get full name.
  state STRING,
  -- If the `state` is uninterruptible sleep, `io_wait` indicates if it was
  -- an IO sleep. Will be null if `state` is *not* uninterruptible sleep or if
  -- we cannot tell if it was an IO sleep or not.
  --
  -- Only available on Android when
  -- `sched/sched_blocked_reason` ftrace tracepoint is enabled.
  io_wait BOOL,
  -- If in uninterruptible sleep (D), the kernel function on which was blocked.
  -- Only available on userdebug Android builds when
  -- `sched/sched_blocked_reason` ftrace tracepoint is enabled.
  blocked_function STRING,
  -- The duration of time the threads slice spent for each
  -- (state, io_wait, blocked_function) tuple.
  dur DURATION
) MATERIALIZED AS
SUBPIPELINE ts AS (
  FROM thread_slice
  |> WHERE utid > 0 AND dur > 0
)
SUBPIPELINE tstate AS (
  FROM thread_state
  |> WHERE dur > 0
)
INTERVAL INTERSECTION OF (ts AS ts, tstate AS tstate) PER utid
|> AGGREGATE SUM(dur) AS dur
   GROUP BY ts.id, tstate.state, tstate.io_wait, tstate.blocked_function
|> SELECT
     ts.id AS id,
     ts.name,
     ts.utid,
     ts.thread_name,
     ts.upid,
     ts.process_name,
     tstate.state,
     tstate.io_wait,
     tstate.blocked_function,
     dur
|> ORDER BY ts.id;
