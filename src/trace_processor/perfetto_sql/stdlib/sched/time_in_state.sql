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

INCLUDE PERFETTO MODULE intervals.intersect;

-- NOTE (psqlnext): the `*_in_interval` constructs are parameterized by a scalar
-- window (plus a key), so they are pipeline-valued macros (`RETURNS Pipeline`)
-- clipping the keyed relation with `_interval_intersect_single!($ts, $dur, …)`.

-- The time a thread spent in each scheduling state during it's lifetime.
CREATE PERFETTO PIPELINE sched_time_in_state_for_thread(
  -- Utid of the thread.
  utid JOINID(thread.id),
  -- Total runtime of thread.
  total_runtime LONG,
  -- One of the scheduling states of kernel thread.
  state STRING,
  -- Total time spent in the scheduling state.
  time_in_state LONG,
  -- Percentage of time thread spent in scheduling state in [0-100] range.
  percentage_in_state LONG
) MATERIALIZED AS
SUBPIPELINE total_dur AS (
  FROM thread_state
  |> AGGREGATE sum(dur) AS sum_dur GROUP BY utid
)
FROM thread_state
|> AGGREGATE sum(dur) AS time_in_state GROUP BY utid, state
|> JOIN total_dur USING (utid)
|> SELECT
     utid,
     sum_dur AS total_runtime,
     state,
     time_in_state,
     (time_in_state * 100) / (sum_dur) AS percentage_in_state;

CREATE PERFETTO MACRO _case_for_state(state Expr)
RETURNS Expr
AS max(CASE WHEN state = $state THEN percentage_in_state END);

-- Summary of time spent by thread in each scheduling state, in percentage ([0, 100]
-- ranges). Sum of all states might be smaller than 100, as those values
-- are rounded down.
CREATE PERFETTO PIPELINE sched_percentage_of_time_in_state(
  -- Utid of the thread.
  utid JOINID(thread.id),
  -- Percentage of time thread spent in running ('Running') state in [0, 100]
  -- range.
  running LONG,
  -- Percentage of time thread spent in runnable ('R') state in [0, 100]
  -- range.
  runnable LONG,
  -- Percentage of time thread spent in preempted runnable ('R+') state in
  -- [0, 100] range.
  runnable_preempted LONG,
  -- Percentage of time thread spent in sleeping ('S') state in [0, 100] range.
  sleeping LONG,
  -- Percentage of time thread spent in uninterruptible sleep ('D') state in
  -- [0, 100] range.
  uninterruptible_sleep LONG,
  -- Percentage of time thread spent in other ('T', 't', 'X', 'Z', 'x', 'I',
  -- 'K', 'W', 'P', 'N') states in [0, 100] range.
  other LONG
) MATERIALIZED AS
FROM sched_time_in_state_for_thread
|> AGGREGATE
     _case_for_state!('Running') AS running,
     _case_for_state!('R') AS runnable,
     _case_for_state!('R+') AS runnable_preempted,
     _case_for_state!('S') AS sleeping,
     _case_for_state!('D') AS uninterruptible_sleep,
     sum(
       CASE
         WHEN state IN ('T', 't', 'X', 'Z', 'x', 'I', 'K', 'W', 'P', 'N') THEN time_in_state
       END
     )
     * 100
     / ANY_VALUE(total_runtime) AS other
   GROUP BY utid;

-- Time the thread spent each state in a given interval.
--
-- This function is only designed to run over a small number of intervals
-- (10-100 at most). It will be *very slow* for large sets of intervals.
--
-- Specifically for any non-trivial subset of thread slices, prefer using
-- `thread_slice_time_in_state` in the `slices.time_in_state` module for this
-- purpose instead.
CREATE PERFETTO MACRO sched_time_in_state_for_thread_in_interval(
  -- The start of the interval.
  ts Expr,
  -- The duration of the interval.
  dur Expr,
  -- The utid of the thread.
  utid Expr
)
-- Returns: (state STRING, io_wait BOOL, blocked_function LONG, dur DURATION).
RETURNS Pipeline AS (
  _interval_intersect_single!($ts, $dur, (
    SELECT id, ts, dur, state, io_wait, blocked_function
    FROM thread_state
    WHERE utid = $utid AND dur > 0
  ))
  |> AGGREGATE
       sum(dur) AS dur
     GROUP BY state, io_wait, blocked_function
  |> ORDER BY dur DESC
);

-- Time the thread spent each state and cpu in a given interval.
--
-- This function is only designed to run over a small number of intervals
-- (10-100 at most). It will be *very slow* for large sets of intervals.
CREATE PERFETTO MACRO sched_time_in_state_and_cpu_for_thread_in_interval(
  -- The start of the interval.
  ts Expr,
  -- The duration of the interval.
  dur Expr,
  -- The utid of the thread.
  utid Expr
)
-- Returns: (state STRING, io_wait BOOL, cpu LONG, blocked_function LONG,
-- dur DURATION).
RETURNS Pipeline AS (
  _interval_intersect_single!($ts, $dur, (
    SELECT id, ts, dur, state, io_wait, cpu, blocked_function
    FROM thread_state
    WHERE utid = $utid AND dur > 0
  ))
  |> AGGREGATE
       sum(dur) AS dur
     GROUP BY state, io_wait, cpu, blocked_function
  |> ORDER BY dur DESC
);

-- Time spent by CPU in each scheduling state in a provided interval.
--
-- This function is only designed to run over a small number of intervals
-- (10-100 at most). It will be *very slow* for large sets of intervals.
CREATE PERFETTO MACRO sched_time_in_state_for_cpu_in_interval(
  -- CPU id.
  cpu Expr,
  -- Interval start.
  ts Expr,
  -- Interval duration.
  dur Expr
)
-- Returns: (end_state STRING, dur LONG).
RETURNS Pipeline AS (
  _interval_intersect_single!($ts, $dur, (
    SELECT id, ts, dur, end_state
    FROM sched
    WHERE cpu = $cpu AND dur != -1
  ))
  |> AGGREGATE sum(dur) AS dur GROUP BY end_state
);
