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

-- NOTE (psqlnext): `intervals_overlap_count!` is `INTERVAL FLATTEN COUNT(*)`
-- (the disjoint concurrency over covered time) plus `INTERVAL FILL WITHIN
-- trace_bounds` to materialize the zero-runs where no interval is open. The
-- result is projected to the legacy `(ts, value)` point form.

-- The count of runnable threads over time.
CREATE PERFETTO PIPELINE sched_runnable_thread_count(
  -- Timestamp when the runnable thread count changed to the current value.
  ts TIMESTAMP,
  -- Number of runnable threads, covering the range from this timestamp to the
  -- next row's timestamp.
  runnable_thread_count LONG
) MATERIALIZED AS
FROM thread_state
|> WHERE state = 'R'
|> SELECT ts, dur
|> INTERVAL FLATTEN COUNT(*) AS value
|> INTERVAL FILL WITHIN trace_bounds
|> SELECT ts, COALESCE(value, 0) AS runnable_thread_count
|> ORDER BY ts;

-- The count of threads in uninterruptible sleep over time.
CREATE PERFETTO PIPELINE sched_uninterruptible_sleep_thread_count(
  -- Timestamp when the thread count changed to the current value.
  ts TIMESTAMP,
  -- Number of threads in uninterrutible sleep, covering the range from this timestamp to the
  -- next row's timestamp.
  uninterruptible_sleep_thread_count LONG
) MATERIALIZED AS
FROM thread_state
|> WHERE state = 'D'
|> SELECT ts, dur
|> INTERVAL FLATTEN COUNT(*) AS value
|> INTERVAL FILL WITHIN trace_bounds
|> SELECT ts, COALESCE(value, 0) AS uninterruptible_sleep_thread_count
|> ORDER BY ts;

-- The count of active CPUs over time.
CREATE PERFETTO PIPELINE sched_active_cpu_count(
  -- Timestamp when the number of active CPU changed.
  ts TIMESTAMP,
  -- Number of active CPUs, covering the range from this timestamp to the next
  -- row's timestamp.
  active_cpu_count LONG
) MATERIALIZED AS
-- Filter sched events corresponding to running tasks.
-- thread(s) with is_idle = 1 are the swapper threads / idle tasks.
FROM sched
|> WHERE NOT (utid IN (SELECT utid FROM thread WHERE is_idle))
|> SELECT ts, dur
|> INTERVAL FLATTEN COUNT(*) AS value
|> INTERVAL FILL WITHIN trace_bounds
|> SELECT ts, COALESCE(value, 0) AS active_cpu_count
|> ORDER BY ts;
