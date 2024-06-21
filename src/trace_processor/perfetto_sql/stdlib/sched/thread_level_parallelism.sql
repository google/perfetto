--
-- Copyright 2023 The Android Open Source Project
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

-- This module contains helpers for computing the thread-level parallelism counters,
-- including how many threads were runnable at a given time and how many threads
-- where running at a given point in time.

INCLUDE PERFETTO MODULE intervals.overlap;

-- The count of runnable threads over time.
CREATE PERFETTO TABLE sched_runnable_thread_count(
  -- Timestamp when the runnable thread count changed to the current value.
  ts INT,
  -- Number of runnable threads, covering the range from this timestamp to the
  -- next row's timestamp.
  runnable_thread_count INT
) AS
WITH
runnable AS (
  SELECT ts, dur FROM thread_state
  where state = 'R'
)
SELECT
  ts, value as runnable_thread_count
FROM intervals_overlap_count!(runnable, ts, dur)
ORDER BY ts;

-- The count of active CPUs over time.
CREATE PERFETTO TABLE sched_active_cpu_count(
  -- Timestamp when the number of active CPU changed.
  ts INT,
  -- Number of active CPUs, covering the range from this timestamp to the next
  -- row's timestamp.
  active_cpu_count INT
) AS
WITH
-- Filter sched events corresponding to running tasks.
-- utid=0 is the swapper thread / idle task.
tasks AS (
  SELECT ts, dur
  FROM sched
  WHERE utid != 0
)
SELECT
  ts, value as active_cpu_count
FROM intervals_overlap_count!(tasks, ts, dur)
ORDER BY ts;
