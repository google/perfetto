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

-- Turns intervals flattened per machine, with the gaps filled, into a counter
-- with one row at each `ts` where an interval starts or ends.
--
-- Unfinished intervals (`dur = -1`) must end at `trace_end() + 1`, so they are
-- still counted at `trace_end()`, and the gaps be filled until then. A point
-- (`dur = 0`) gets a row but is not counted. The filler before the first
-- interval of each machine is dropped.
CREATE PERFETTO MACRO _sched_flattened_intervals_to_counter(
  -- Output of `INTERVAL FLATTEN PER machine_id AGGREGATE COUNT(*) AS c,
  -- SUM(is_point) AS points` followed by `INTERVAL FILL GAPS`.
  flattened TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  WITH
    first_ts AS MATERIALIZED (
      SELECT machine_id, min(ts) AS ts
      FROM $flattened
      WHERE
        c IS NOT NULL
      GROUP BY
        machine_id
    )
  SELECT
    f.ts,
    f.machine_id,
    max(coalesce(f.c, 0) - coalesce(f.points, 0)) AS value
  FROM $flattened AS f
  JOIN first_ts AS s
    ON f.machine_id IS s.machine_id AND f.ts >= s.ts
  GROUP BY
    f.machine_id,
    f.ts
  ORDER BY
    f.machine_id,
    f.ts
);

CREATE PERFETTO TABLE _sched_runnable_thread_flattened AS
FROM (
  SELECT
    ts,
    iif(dur = -1, trace_end() + 1 - ts, dur) AS dur,
    thread_state.dur = 0 AS is_point,
    thread.machine_id
  FROM thread_state
  JOIN thread USING (utid)
  WHERE
    state = 'R'
)
|> INTERVAL FLATTEN PER machine_id
  AGGREGATE COUNT(*) AS c,
  SUM(is_point) AS points
|> INTERVAL FILL GAPS WITH (SELECT trace_start() AS ts, trace_dur() + 1 AS dur) PER machine_id;

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
SELECT ts, machine_id, value AS runnable_thread_count
FROM _sched_flattened_intervals_to_counter!(_sched_runnable_thread_flattened);

CREATE PERFETTO TABLE _sched_uninterruptible_sleep_thread_flattened AS
FROM (
  SELECT
    ts,
    iif(dur = -1, trace_end() + 1 - ts, dur) AS dur,
    thread_state.dur = 0 AS is_point,
    thread.machine_id
  FROM thread_state
  JOIN thread USING (utid)
  WHERE
    state = 'D'
)
|> INTERVAL FLATTEN PER machine_id
  AGGREGATE COUNT(*) AS c,
  SUM(is_point) AS points
|> INTERVAL FILL GAPS WITH (SELECT trace_start() AS ts, trace_dur() + 1 AS dur) PER machine_id;

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
SELECT ts, machine_id, value AS uninterruptible_sleep_thread_count
FROM _sched_flattened_intervals_to_counter!(
  _sched_uninterruptible_sleep_thread_flattened
);

-- Filter sched events corresponding to running tasks.
-- thread(s) with is_idle = 1 are the swapper threads / idle tasks.
CREATE PERFETTO TABLE _sched_active_cpu_flattened AS
FROM (
  SELECT
    sched.ts,
    iif(sched.dur = -1, trace_end() + 1 - sched.ts, sched.dur) AS dur,
    sched.dur = 0 AS is_point,
    cpu.machine_id
  FROM sched
  JOIN cpu USING (ucpu)
  WHERE
    NOT (utid IN (SELECT utid FROM thread WHERE is_idle))
)
|> INTERVAL FLATTEN PER machine_id
  AGGREGATE COUNT(*) AS c,
  SUM(is_point) AS points
|> INTERVAL FILL GAPS WITH (SELECT trace_start() AS ts, trace_dur() + 1 AS dur) PER machine_id;

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
SELECT ts, machine_id, value AS active_cpu_count
FROM _sched_flattened_intervals_to_counter!(_sched_active_cpu_flattened);
