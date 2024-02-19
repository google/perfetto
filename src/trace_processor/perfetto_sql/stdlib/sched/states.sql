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

-- Translates the thread state name from a single-letter shorthard to
-- a human-readable name.
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

-- Creates a table with humanly readable sched state names with IO waits.
-- Translates the individual characters in the string to the following: R
-- (runnable), S (awaiting a wakeup), D (in an uninterruptible sleep), T
-- (suspended), t (being traced), X (exiting), P (parked), W (waking), I
-- (idle), N (not contributing to the load average), K (wakeable on fatal
-- signals) and Z (zombie, awaiting cleanup). Adds the IO wait (IO/non
-- IO/nothing) based on the value in the `io_wait_column`.
CREATE PERFETTO MACRO sched_state_full_name(
  -- Table with columns required for translation and `id` column for joins.
  states_table TableOrSubquery,
  -- Column in `states_table` with single character version of sched state
  -- string.
  sched_name_column ColumnName,
  -- Column in `states_table` with 0 for IO and 1 for non-IO states. Can be
  -- a dummy (no real values), and no value from there would be added to the
  -- resulting strings.
  io_wait_column ColumnName
)
-- Table with the schema (id UINT32, ts UINT64, sched_state_full_name STRING).
RETURNS TableOrSubquery AS
(
  WITH data AS
  (
    SELECT
      id,
      sched_state_to_human_readable_string($sched_name_column) AS sched_state_name,
      (CASE $io_wait_column
        WHEN 1 THEN ' (IO)'
        WHEN 0 THEN ' (non-IO)'
        ELSE ''
      END) AS io_wait
    FROM $states_table
  )
  SELECT
    id,
    printf('%s%s', sched_state_name, io_wait) AS sched_state_full_name
  FROM data
);
