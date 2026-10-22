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
--

-- A 'thread_executing_span' is thread_state span starting with a runnable slice
-- until the next runnable slice that's woken up by a process (as opposed
-- to an interrupt). Note that within a 'thread_executing_span' we can have sleep
-- spans blocked on an interrupt.
-- We consider the id of this span to be the id of the first thread_state in the span.

--
-- Finds all runnable states that are woken up by a process.
--
-- We achieve this by checking that the |thread_state.irq_context|
-- value is NOT 1. In otherwords, it is either 0 or NULL. The NULL check
-- is important to support older Android versions.
--
-- On older versions of Android (<U). We don't have IRQ context information,
-- so this table might contain wakeups from interrupt context, consequently, the
-- wakeup graph generated might not be accurate.
--
CREATE PERFETTO TABLE _runnable_state AS
SELECT
  thread_state.id,
  thread_state.ts,
  thread_state.dur,
  thread_state.state,
  thread_state.utid,
  thread_state.waker_id,
  thread_state.waker_utid,
  iif(
    thread_state.irq_context = 0
    OR thread_state.irq_context IS NULL,
    coalesce(thread_state.io_wait, 0),
    1
  ) AS is_irq
FROM thread_state
WHERE
  thread_state.dur != -1
  AND thread_state.waker_id IS NOT NULL;

-- Defines if the trace is a Fuchsia trace.
CREATE PERFETTO TABLE _is_fuchsia AS
SELECT
  EXISTS (
    SELECT 1 FROM metadata WHERE name = 'trace_type' AND str_value = 'fuchsia'
  ) AS is_fuchsia;

-- Check whether a given scheduler event is for a task becoming runnable.
-- Linux always has a Waking duration *and* Runnable before each Running duration.
-- Fuchsia only has a Waking duration before the first Running duration, Runnable durations
-- are used if the initial activation's timeslice expires or is interrupted.
CREATE PERFETTO MACRO _is_runnable_state(state Expr)
RETURNS Expr
AS $state = 'R'
OR (
  $state = 'W' AND (
    SELECT
      is_fuchsia
    FROM _is_fuchsia
  )
);

-- Similar to |_runnable_state| but finds the first runnable state at thread.
CREATE PERFETTO TABLE _first_runnable_state AS
WITH
  first_state AS (
    SELECT min(thread_state.id) AS id FROM thread_state GROUP BY utid
  )
SELECT
  thread_state.id,
  thread_state.ts,
  thread_state.dur,
  thread_state.state,
  thread_state.utid,
  thread_state.waker_id,
  thread_state.waker_utid,
  iif(
    thread_state.irq_context = 0
    OR thread_state.irq_context IS NULL,
    coalesce(thread_state.io_wait, 0),
    1
  ) AS is_irq
FROM thread_state
JOIN first_state USING (id)
WHERE
  thread_state.dur != -1
  AND _is_runnable_state!(thread_state.state);

--
-- Finds all sleep states including interruptible (S) and uninterruptible (D).
CREATE PERFETTO TABLE _sleep_state AS
SELECT
  thread_state.id,
  thread_state.ts,
  thread_state.dur,
  thread_state.state,
  thread_state.blocked_function,
  thread_state.utid
FROM thread_state
WHERE
  dur != -1
  AND (state = 'S' OR state = 'D' OR state = 'I');

--
-- Finds the last execution for every thread to end executing_spans without a Sleep.
--
CREATE PERFETTO TABLE _thread_end_ts AS
SELECT max(ts) + dur AS end_ts, utid
FROM thread_state
WHERE
  dur != -1
GROUP BY
  utid;

-- Similar to |_sleep_state| but finds the first sleep state in a thread.
CREATE PERFETTO TABLE _first_sleep_state AS
SELECT min(s.id) AS id, s.ts, s.dur, s.state, s.blocked_function, s.utid
FROM _sleep_state AS s
JOIN _runnable_state AS r
  ON s.utid = r.utid
  AND (s.ts + s.dur = r.ts)
GROUP BY
  s.utid;

--
-- Finds all neighbouring ('Sleeping', 'Runnable') thread_states pairs from the same thread.
-- More succintly, pairs of S[n-1]-R[n] where R is woken by a process context and S is an
-- interruptible or uninterruptible sleep state.
--
-- This is achieved by joining the |_runnable_state|.ts with the
-- |_sleep_state|.|ts + dur|.
--
-- With the S-R pairs of a thread, we can re-align to [R-S) intervals with LEADS and LAGS.
--
-- Given the following thread_states on a thread:
-- S0__|R0__Running0___|S1__|R1__Running1___|S2__|R2__Running2__S2|.
--
-- We have 3 thread_executing_spans: [R0, S0), [R1, S1), [R2, S2).
--
-- We define the following markers in this table:
--
-- prev_id          = R0_id.
--
-- prev_end_ts      = S0_ts.
-- state            = 'S' or 'D'.
-- blocked_function = <kernel blocking function>
--
-- id               = R1_id.
-- ts               = R1_ts.
--
-- end_ts           = S1_ts.
CREATE PERFETTO TABLE _wakeup AS
WITH
  all_wakeups AS (
    SELECT
      s.state,
      s.blocked_function,
      r.id,
      r.ts AS ts,
      r.utid AS utid,
      r.waker_id,
      r.waker_utid,
      s.ts AS prev_end_ts,
      is_irq
    FROM _runnable_state AS r
    JOIN _sleep_state AS s
      ON s.utid = r.utid
      AND (s.ts + s.dur = r.ts)
    UNION ALL
    SELECT
      NULL AS state,
      NULL AS blocked_function,
      r.id,
      r.ts,
      r.utid AS utid,
      r.waker_id,
      r.waker_utid,
      NULL AS prev_end_ts,
      is_irq
    FROM _first_runnable_state AS r
    LEFT JOIN _first_sleep_state AS s
      ON s.utid = r.utid
  )
SELECT all_wakeups.*, thread_end.end_ts AS thread_end_ts
FROM all_wakeups
LEFT JOIN _thread_end_ts AS thread_end USING (utid);

-- Mapping from running thread state to runnable
-- TODO(zezeozue): Switch to use `sched_previous_runnable_on_thread`.
CREATE PERFETTO TABLE _wakeup_map AS
WITH
  x AS (
    SELECT id, waker_id, utid, state
    FROM thread_state
    WHERE
      state = 'Running'
      AND dur != -1
    UNION ALL
    SELECT id, waker_id, utid, state FROM _first_runnable_state
    UNION ALL
    SELECT id, waker_id, utid, state FROM _runnable_state
  ),
  y AS (
    SELECT
      id AS waker_id,
      state,
      max(id) FILTER (WHERE _is_runnable_state!(state)) OVER (
        PARTITION BY
          utid
        ORDER BY id
      ) AS id
    FROM x
  )
SELECT id, waker_id FROM y WHERE state = 'Running' ORDER BY waker_id;

--
-- Builds the waker and prev relationships for all thread_executing_spans.
--
CREATE PERFETTO TABLE _wakeup_graph AS
WITH
  _wakeup_events AS (
    SELECT
      utid,
      thread_end_ts,
      iif(is_irq, 'IRQ', state) AS idle_state,
      blocked_function AS idle_reason,
      _wakeup.id,
      iif(is_irq, NULL, _wakeup_map.id) AS waker_id,
      _wakeup.ts,
      prev_end_ts AS idle_ts
    FROM _wakeup
    LEFT JOIN _wakeup_map USING (waker_id)
  )
SELECT
  utid,
  id,
  waker_id,
  ts,
  idle_state,
  idle_reason,
  ts - idle_ts AS idle_dur,
  lag(id) OVER (PARTITION BY utid ORDER BY ts) AS prev_id,
  coalesce(lead(idle_ts) OVER (PARTITION BY utid ORDER BY ts), thread_end_ts)
  - ts AS dur
FROM _wakeup_events
ORDER BY
  id;

-- Piece-wise constant functional map of thread state intervals:
-- Every interval [ts, ts + dur) on `utid` either terminates (`target_utid` is
-- NULL when running or in unattributed/IRQ sleep) or delegates to `target_utid`
-- (when sleeping before a process-context wakeup).
CREATE PERFETTO TABLE _wait_edge AS
FROM (
  SELECT
    utid,
    ts,
    dur,
    CAST(NULL AS LONG) AS target_utid,
    id AS node_id,
    0 AS is_open_idle
  FROM _wakeup_graph
  WHERE
    dur > 0
  UNION ALL
  SELECT
    g.utid,
    g.ts - g.idle_dur AS ts,
    g.idle_dur AS dur,
    w.utid AS target_utid,
    g.id AS node_id,
    0 AS is_open_idle
  FROM _wakeup_graph AS g
  LEFT JOIN _wakeup_graph AS w
    ON w.id = g.waker_id
  WHERE
    g.idle_dur > 0
    AND (g.prev_id IS NOT NULL OR w.id IS NOT NULL)
  UNION ALL
  SELECT
    g.utid,
    trace_start() AS ts,
    iif(w.id IS NOT NULL AND g.idle_dur > 0, g.ts - g.idle_dur, g.ts)
    - trace_start() AS dur,
    w.utid AS target_utid,
    g.id AS node_id,
    1 AS is_open_idle
  FROM _wakeup_graph AS g
  LEFT JOIN _wakeup_graph AS w
    ON w.id = g.waker_id
  WHERE
    g.prev_id IS NULL
    AND iif(w.id IS NOT NULL AND g.idle_dur > 0, g.ts - g.idle_dur, g.ts)
    > trace_start()
)
|> ORDER BY utid, ts;

-- Every interval slice of the trace resolved to the leaf thread (`utid`) and
-- executing span (`id`) that `root_utid` was transitively waiting on.
CREATE PERFETTO TABLE _critical_path_all AS
FROM _wait_edge
|> WHERE is_open_idle = 0
|> EXTEND utid AS root_utid, node_id AS root_id
|> INTERVAL WALK WITH _wait_edge PER utid TO target_utid
|> SELECT root_utid, root_id, node_id AS id, ts, dur, utid;

-- Generates the critical path for a given utid over the <ts, dur> interval.
-- The duration of a thread executing span in the critical path is the range between the
-- start of the thread_executing_span and the start of the next span in the critical path.
CREATE PERFETTO FUNCTION _thread_executing_span_critical_path(
  -- Utid of the thread to compute the critical path for.
  root_utid JOINID(thread.id),
  -- Timestamp.
  ts TIMESTAMP,
  -- Duration.
  dur DURATION
)
RETURNS TABLE(
  -- Thread Utid the critical path was filtered to.
  root_utid JOINID(thread.id),
  -- Id of thread executing span following the sleeping thread state for which the critical path is
  -- computed.
  root_id LONG,
  -- Id of the first (runnable) thread state in thread_executing_span.
  id LONG,
  -- Timestamp of first thread_state in thread_executing_span.
  ts TIMESTAMP,
  -- Duration of thread_executing_span.
  dur DURATION,
  -- Utid of thread with thread_state.
  utid JOINID(thread.id)
)
AS
SELECT
  root_utid,
  root_id,
  id,
  max(ts, $ts) AS ts,
  min(ts + dur, $ts + $dur) - max(ts, $ts) AS dur,
  utid
FROM _critical_path_all
WHERE
  root_utid = $root_utid
  AND ts < $ts + $dur
  AND ts + dur > $ts;
