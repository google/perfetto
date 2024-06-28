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

-- The time a thread spent in each scheduling state during it's lifetime.
CREATE PERFETTO TABLE sched_time_in_state_for_thread(
  -- Utid of the thread.
  utid INT,
  -- Total runtime of thread.
  total_runtime INT,
  -- One of the scheduling states of kernel thread.
  state STRING,
  -- Total time spent in the scheduling state.
  time_in_state INT,
  -- Percentage of time thread spent in scheduling state in [0-100] range.
  percentage_in_state INT
) AS
WITH total_dur AS (
  SELECT
    utid,
    sum(dur) AS sum_dur
  FROM thread_state
  GROUP BY 1
),
summed AS (
  SELECT
    utid,
    state,
    sum(dur) AS time_in_state
  FROM thread_state group by 1, 2
)
SELECT
  utid,
  sum_dur AS total_runtime,
  state,
  time_in_state,
  (time_in_state*100)/(sum_dur) AS percentage_in_state
FROM summed JOIN total_dur USING (utid);

CREATE PERFETTO MACRO _case_for_state(state Expr)
RETURNS Expr AS
MAX(CASE WHEN state = $state THEN percentage_in_state END);

-- Summary of time spent by thread in each scheduling state, in percentage ([0, 100]
-- ranges). Sum of all states might be smaller than 100, as those values
-- are rounded down.
CREATE PERFETTO TABLE sched_percentage_of_time_in_state(
  -- Utid of the thread.
  utid INT,
  -- Percentage of time thread spent in running ('Running') state in [0, 100]
  -- range.
  running INT,
  -- Percentage of time thread spent in runnable ('R') state in [0, 100]
  -- range.
  runnable INT,
  -- Percentage of time thread spent in preempted runnable ('R+') state in
  -- [0, 100] range.
  runnable_preempted INT,
  -- Percentage of time thread spent in sleeping ('S') state in [0, 100] range.
  sleeping INT,
  -- Percentage of time thread spent in uninterruptible sleep ('D') state in
  -- [0, 100] range.
  uninterruptible_sleep INT,
  -- Percentage of time thread spent in other ('T', 't', 'X', 'Z', 'x', 'I',
  -- 'K', 'W', 'P', 'N') states in [0, 100] range.
  other INT
) AS
SELECT
  utid,
  _case_for_state!('Running') AS running,
  _case_for_state!('R') AS runnable,
  _case_for_state!('R+') AS runnable_preempted,
  _case_for_state!('S') AS sleeping,
  _case_for_state!('D') AS uninterruptible_sleep,
  SUM(
    CASE WHEN state IN ('T', 't', 'X', 'Z', 'x', 'I', 'K', 'W', 'P', 'N')
    THEN time_in_state END
  ) * 100/total_runtime AS other
FROM sched_time_in_state_for_thread
GROUP BY utid;

-- Time the thread spent each state in a given interval.
CREATE PERFETTO FUNCTION sched_time_in_state_for_thread_in_interval(
  -- The start of the interval.
  ts INT,
  -- The duration of the interval.
  dur INT,
  -- The utid of the thread.
  utid INT)
RETURNS TABLE(
  -- Thread state (from the `thread_state` table).
  -- Use `sched_state_to_human_readable_string` function to get full name.
  state INT,
  -- A (posssibly NULL) boolean indicating, if the device was in uninterruptible
  -- sleep, if it was an IO sleep.
  io_wait BOOL,
  -- Some states can specify the blocked function. Usually NULL.
  blocked_function INT,
  -- Total time spent with this state, cpu and blocked function.
  dur INT) AS
SELECT
  state,
  io_wait,
  blocked_function,
  sum(ii.dur) as dur
FROM thread_state
JOIN
  (SELECT * FROM _interval_intersect_single!(
    $ts, $dur,
    (SELECT id, ts, dur
    FROM thread_state
    WHERE utid = $utid AND dur > 0))) ii USING (id)
GROUP BY 1, 2, 3
ORDER BY 4 DESC;

-- Time the thread spent each state and cpu in a given interval.
CREATE PERFETTO FUNCTION sched_time_in_state_and_cpu_for_thread_in_interval(
  -- The start of the interval.
  ts INT,
  -- The duration of the interval.
  dur INT,
  -- The utid of the thread.
  utid INT)
RETURNS TABLE(
  -- Thread state (from the `thread_state` table).
  -- Use `sched_state_to_human_readable_string` function to get full name.
  state INT,
  -- A (posssibly NULL) boolean indicating, if the device was in uninterruptible
  -- sleep, if it was an IO sleep.
  io_wait BOOL,
  -- Id of the CPU.
  cpu INT,
  -- Some states can specify the blocked function. Usually NULL.
  blocked_function INT,
  -- Total time spent with this state, cpu and blocked function.
  dur INT) AS
SELECT
  state,
  io_wait,
  cpu,
  blocked_function,
  sum(ii.dur) as dur
FROM thread_state
JOIN
  (SELECT * FROM _interval_intersect_single!(
    $ts, $dur,
    (SELECT id, ts, dur
    FROM thread_state
    WHERE utid = $utid AND dur > 0))) ii USING (id)
GROUP BY 1, 2, 3, 4
ORDER BY 5 DESC;

-- Time spent by CPU in each scheduling state in a provided interval.
CREATE PERFETTO FUNCTION sched_time_in_state_for_cpu_in_interval(
    -- CPU id.
    cpu INT,
    -- Interval start.
    ts INT,
    -- Interval duration.
    dur INT
) RETURNS TABLE (
    -- End state. From `sched.end_state`.
    end_state STRING,
    -- Duration in state.
    dur INT
) AS
WITH sched_for_cpu AS (
  SELECT id, ts, dur
  FROM sched
  WHERE cpu = $cpu AND dur != -1
)
SELECT
    end_state,
    sum(ii.dur) AS dur
FROM sched
JOIN _interval_intersect_single!($ts, $dur, sched_for_cpu) ii
USING (id)
GROUP BY end_state;



