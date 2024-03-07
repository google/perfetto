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

-- TODO(altimin): `sched_humanly_readable_name` doesn't handle some corner
-- cases which thread_state.ts handles (as complex strings manipulations in
-- SQL are pretty painful), but they are pretty niche.

-- Translates a single-letter scheduling state to a human-readable string.
CREATE PERFETTO FUNCTION sched_state_to_human_readable_string(
  -- An individual character string representing the scheduling state of the
  -- kernel thread at the end of the slice.
  short_name STRING
)
-- Humanly readable string representing the scheduling state of the kernel
-- thread. The individual characters in the string mean the following: R
-- (runnable), S (awaiting a wakeup), D (in an uninterruptible sleep), T
-- (suspended), t (being traced), X (exiting), P (parked), W (waking), I
-- (idle), N (not contributing to the load average), K (wakeable on fatal
-- signals) and Z (zombie, awaiting cleanup).
RETURNS STRING AS
SELECT CASE $short_name
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
ELSE $short_name
END;

-- Translates a single-letter scheduling state and IO wait information to
-- a human-readable string.
CREATE PERFETTO FUNCTION sched_state_io_to_human_readable_string(
  -- An individual character string representing the scheduling state of the
  -- kernel thread at the end of the slice.
  sched_state STRING,
  -- A (posssibly NULL) boolean indicating, if the device was in uninterruptible
  -- sleep, if it was an IO sleep.
  io_wait BOOL
)
-- A human readable string with information about the scheduling state and IO wait.
RETURNS STRING AS
SELECT printf(
  '%s%s',
  sched_state_to_human_readable_string($sched_state),
  CASE $io_wait
    WHEN 1 THEN ' (IO)'
    WHEN 0 THEN ' (non-IO)'
    ELSE ''
  END
);

-- The time a thread spent in each scheduling state during it's lifetime.
CREATE PERFETTO TABLE sched_thread_time_in_state(
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
FROM sched_thread_time_in_state
GROUP BY utid;