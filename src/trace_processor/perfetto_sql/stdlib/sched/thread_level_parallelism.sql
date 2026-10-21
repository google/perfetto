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

-- Each counter below starts at its machine's first interval and drops to 0 in
-- every gap after that. Unfinished intervals (`dur = -1`) are extended to
-- `trace_end() + 1`, so they are still counted at `trace_end()`.

CREATE PERFETTO TABLE _sched_runnable_thread_intervals AS
SELECT ts, iif(dur = -1, trace_end() + 1 - ts, dur) AS dur, thread.machine_id
FROM thread_state
JOIN thread USING (utid)
WHERE
  state = 'R'
  AND dur != 0;

-- The count of runnable threads over time.
CREATE PERFETTO TABLE sched_runnable_thread_count(
  -- Timestamp when the runnable thread count changed to the current value.
  ts TIMESTAMP,
  -- Machine whose runnable threads are counted.
  machine_id JOINID(machine.id),
  -- Number of runnable threads, covering the range from this timestamp to the
  -- next row's timestamp.
  runnable_thread_count LONG
)
AS
FROM _sched_runnable_thread_intervals
|> INTERVAL FLATTEN PER machine_id AGGREGATE COUNT(*) AS c
|> INTERVAL FILL GAPS WITH (
  SELECT machine_id, min(ts) AS ts, trace_end() + 1 - min(ts) AS dur, 0 AS c
  FROM _sched_runnable_thread_intervals
  GROUP BY
    machine_id
) PER machine_id
|> SELECT ts, machine_id, c AS runnable_thread_count
|> ORDER BY machine_id, ts;

CREATE PERFETTO TABLE _sched_uninterruptible_sleep_thread_intervals AS
SELECT ts, iif(dur = -1, trace_end() + 1 - ts, dur) AS dur, thread.machine_id
FROM thread_state
JOIN thread USING (utid)
WHERE
  state = 'D'
  AND dur != 0;

-- The count of threads in uninterruptible sleep over time.
CREATE PERFETTO TABLE sched_uninterruptible_sleep_thread_count(
  -- Timestamp when the thread count changed to the current value.
  ts TIMESTAMP,
  -- Machine whose threads are counted.
  machine_id JOINID(machine.id),
  -- Number of threads in uninterrutible sleep, covering the range from this timestamp to the
  -- next row's timestamp.
  uninterruptible_sleep_thread_count LONG
)
AS
FROM _sched_uninterruptible_sleep_thread_intervals
|> INTERVAL FLATTEN PER machine_id AGGREGATE COUNT(*) AS c
|> INTERVAL FILL GAPS WITH (
  SELECT machine_id, min(ts) AS ts, trace_end() + 1 - min(ts) AS dur, 0 AS c
  FROM _sched_uninterruptible_sleep_thread_intervals
  GROUP BY
    machine_id
) PER machine_id
|> SELECT ts, machine_id, c AS uninterruptible_sleep_thread_count
|> ORDER BY machine_id, ts;

-- Sched events of running tasks. Threads with is_idle = 1 are the swapper
-- threads / idle tasks.
CREATE PERFETTO TABLE _sched_active_cpu_intervals AS
SELECT
  sched.ts,
  iif(sched.dur = -1, trace_end() + 1 - sched.ts, sched.dur) AS dur,
  cpu.machine_id
FROM sched
JOIN cpu USING (ucpu)
WHERE
  NOT (utid IN (SELECT utid FROM thread WHERE is_idle))
  AND sched.dur != 0;

-- The count of active CPUs over time.
CREATE PERFETTO TABLE sched_active_cpu_count(
  -- Timestamp when the number of active CPU changed.
  ts TIMESTAMP,
  -- Machine whose active CPUs are counted.
  machine_id JOINID(machine.id),
  -- Number of active CPUs, covering the range from this timestamp to the next
  -- row's timestamp.
  active_cpu_count LONG
)
AS
FROM _sched_active_cpu_intervals
|> INTERVAL FLATTEN PER machine_id AGGREGATE COUNT(*) AS c
|> INTERVAL FILL GAPS WITH (
  SELECT machine_id, min(ts) AS ts, trace_end() + 1 - min(ts) AS dur, 0 AS c
  FROM _sched_active_cpu_intervals
  GROUP BY
    machine_id
) PER machine_id
|> SELECT ts, machine_id, c AS active_cpu_count
|> ORDER BY machine_id, ts;
