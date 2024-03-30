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

INCLUDE PERFETTO MODULE graphs.search;

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
CREATE PERFETTO TABLE _runnable_state
AS
SELECT
  thread_state.id,
  thread_state.ts,
  thread_state.dur,
  thread_state.state,
  thread_state.utid,
  thread_state.waker_id,
  thread_state.waker_utid
FROM thread_state
WHERE
  thread_state.dur != -1
  AND thread_state.waker_utid IS NOT NULL
  AND (thread_state.irq_context = 0 OR thread_state.irq_context IS NULL);

-- Similar to |_runnable_state| but finds the first runnable state at thread.
CREATE PERFETTO TABLE _first_runnable_state
AS
WITH
  first_state AS (
    SELECT
      MIN(thread_state.id) AS id
    FROM thread_state
    GROUP BY utid
  )
SELECT
  thread_state.id,
  thread_state.ts,
  thread_state.dur,
  thread_state.state,
  thread_state.utid,
  thread_state.waker_id,
  thread_state.waker_utid
FROM thread_state
JOIN first_state
  USING (id)
WHERE
  thread_state.dur != -1
  AND thread_state.state = 'R'
  AND (thread_state.irq_context = 0 OR thread_state.irq_context IS NULL);

--
-- Finds all sleep states including interruptible (S) and uninterruptible (D).
CREATE PERFETTO TABLE _sleep_state
AS
SELECT
  thread_state.id,
  thread_state.ts,
  thread_state.dur,
  thread_state.state,
  thread_state.blocked_function,
  thread_state.utid
FROM thread_state
WHERE dur != -1 AND (state = 'S' OR state = 'D' OR state = 'I');

--
-- Finds the last execution for every thread to end executing_spans without a Sleep.
--
CREATE PERFETTO TABLE _thread_end_ts
AS
SELECT
  MAX(ts) + dur AS end_ts,
  utid
FROM thread_state
WHERE dur != -1
GROUP BY utid;

-- Similar to |_sleep_state| but finds the first sleep state in a thread.
CREATE PERFETTO TABLE _first_sleep_state
AS
SELECT
  MIN(s.id) AS id,
  s.ts,
  s.dur,
  s.state,
  s.blocked_function,
  s.utid
FROM _sleep_state s
JOIN _runnable_state r
  ON s.utid = r.utid AND (s.ts + s.dur = r.ts)
GROUP BY s.utid;

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
CREATE PERFETTO TABLE _wakeup
AS
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
      s.ts AS prev_end_ts
    FROM _runnable_state r
    JOIN _sleep_state s
      ON s.utid = r.utid AND (s.ts + s.dur = r.ts)
    UNION ALL
    SELECT
      NULL AS state,
      NULL AS blocked_function,
      r.id,
      r.ts,
      r.utid AS utid,
      r.waker_id,
      r.waker_utid,
      NULL AS prev_end_ts
    FROM _first_runnable_state r
    LEFT JOIN _first_sleep_state s
      ON s.utid = r.utid
  )
SELECT
  all_wakeups.*,
  LAG(id) OVER (PARTITION BY utid ORDER BY ts) AS prev_id,
  IFNULL(LEAD(prev_end_ts) OVER (PARTITION BY utid ORDER BY ts), thread_end.end_ts) AS end_ts
FROM all_wakeups
LEFT JOIN _thread_end_ts thread_end
  USING (utid);

-- Mapping from running thread state to runnable
-- TODO(zezeozue): Switch to use `sched_previous_runnable_on_thread`.
CREATE PERFETTO TABLE _wakeup_map
AS
WITH x AS (
SELECT id, waker_id, utid, state FROM thread_state WHERE state = 'Running' AND dur != -1
UNION ALL
SELECT id, waker_id, utid, state FROM _first_runnable_state
UNION ALL
SELECT id, waker_id, utid, state FROM _runnable_state
), y AS (
    SELECT
      id AS waker_id,
      state,
      MAX(id)
        filter(WHERE state = 'R')
          OVER (PARTITION BY utid ORDER BY id) AS id
    FROM x
  )
SELECT id, waker_id FROM y WHERE state = 'Running' ORDER BY waker_id;

--
-- Builds the parent-child chain from all thread_executing_spans. The parent is the waker and
-- child is the wakee.
--
-- Note that this doesn't include the roots. We'll compute the roots below.
-- This two step process improves performance because it's more efficient to scan
-- parent and find a child between than to scan child and find the parent it lies between.
CREATE PERFETTO TABLE _wakeup_graph
AS
SELECT
  _wakeup_map.id AS waker_id,
  prev_id,
  prev_end_ts,
  _wakeup.id AS id,
  _wakeup.ts AS ts,
  _wakeup.end_ts,
  IIF(_wakeup.state IS NULL OR _wakeup.state = 'S', 0, 1) AS is_kernel,
  _wakeup.utid,
  _wakeup.state,
  _wakeup.blocked_function
FROM _wakeup
JOIN _wakeup_map USING(waker_id)
ORDER BY id;

-- The inverse of thread_executing_spans. All the sleeping periods between thread_executing_spans.
CREATE PERFETTO TABLE _sleep
AS
WITH
  x AS (
    SELECT
      id,
      ts,
      prev_end_ts,
      utid,
      state,
      blocked_function
    FROM _wakeup_graph
  )
SELECT
  ts - prev_end_ts AS dur,
  prev_end_ts AS ts,
  id AS root_node_id,
  utid AS critical_path_utid,
  id AS critical_path_id,
  ts - prev_end_ts AS critical_path_blocked_dur,
  state AS critical_path_blocked_state,
  blocked_function AS critical_path_blocked_function
FROM x
WHERE ts IS NOT NULL;

-- Given a set of critical paths identified by their |root_node_ids|, flattens
-- the critical path tasks such that there are no overlapping intervals. The end of a
-- task in the critical path is the start of the following task in the critical path.
CREATE PERFETTO MACRO _flatten_critical_path_tasks(_critical_path_table TableOrSubquery)
RETURNS TableOrSubquery
AS (
  WITH
    x AS (
      SELECT
        LEAD(ts) OVER (PARTITION BY root_node_id ORDER BY node_id) AS ts,
        node_id,
        ts AS node_ts,
        root_node_id,
        utid AS node_utid,
        _wakeup_graph.prev_end_ts
      FROM $_critical_path_table
      JOIN _wakeup_graph
        ON node_id = id
    )
  SELECT node_ts AS ts, root_node_id, node_id, ts - node_ts AS dur, node_utid, prev_end_ts FROM x
);

-- Converts a table with <ts, dur, utid> columns to a unique set of wakeup roots <id> that
-- completely cover the time intervals.
CREATE PERFETTO MACRO _intervals_to_roots(source_table TableOrSubQuery)
RETURNS TableOrSubQuery
AS (
  WITH source AS (
    SELECT * FROM $source_table
  ), thread_bounds AS (
    SELECT utid, MIN(ts) AS min_start, MAX(ts) AS max_start FROM _wakeup_graph GROUP BY utid
  ), start AS (
    SELECT
      _wakeup_graph.utid, max(_wakeup_graph.id) AS start_id, source.ts, source.dur
      FROM _wakeup_graph
      JOIN thread_bounds
        USING (utid)
      JOIN source
        ON source.utid = _wakeup_graph.utid AND MAX(source.ts, min_start) >= _wakeup_graph.ts
     GROUP BY source.ts, source.utid
  ), end AS (
    SELECT
      _wakeup_graph.utid, min(_wakeup_graph.id) AS end_id, source.ts, source.dur
      FROM _wakeup_graph
      JOIN thread_bounds
          USING (utid)
      JOIN source ON source.utid = _wakeup_graph.utid
          AND MIN((source.ts + source.dur), max_start) <= _wakeup_graph.ts
     GROUP BY source.ts, source.utid
  ), bound AS (
    SELECT start.utid, start.ts, start.dur, start_id, end_id
      FROM start
      JOIN end ON start.ts = end.ts AND start.dur = end.dur AND start.utid = end.utid
  )
  SELECT DISTINCT _wakeup_graph.id FROM bound
  JOIN _wakeup_graph ON _wakeup_graph.id BETWEEN start_id AND end_id
);

-- Flattens overlapping tasks within a critical path and flattens overlapping critical paths.
CREATE PERFETTO MACRO _flatten_critical_paths(critical_path_table TableOrSubquery, sleeping_table TableOrSubquery)
RETURNS TableOrSubquery
AS (
  WITH
    span_starts AS (
      SELECT
        cr.node_utid AS utid,
        MAX(cr.ts, sleep.ts) AS ts,
        sleep.ts + sleep.dur AS sleep_end_ts,
        cr.ts + cr.dur AS cr_end_ts,
        cr.node_id AS id,
        cr.root_node_id AS root_id,
        cr.prev_end_ts AS prev_end_ts,
        critical_path_utid,
        critical_path_id,
        critical_path_blocked_dur,
        critical_path_blocked_state,
        critical_path_blocked_function
      FROM
        _flatten_critical_path_tasks!($critical_path_table) cr
      JOIN $sleeping_table sleep
        USING (root_node_id)
    )
  SELECT
    ts,
    MIN(cr_end_ts, sleep_end_ts) - ts AS dur,
    utid,
    id,
    root_id,
    prev_end_ts,
    critical_path_utid,
    critical_path_id,
    critical_path_blocked_dur,
    critical_path_blocked_state,
    critical_path_blocked_function
  FROM span_starts
  WHERE MIN(sleep_end_ts, cr_end_ts) - ts > 0
);

-- Generates a critical path.
CREATE PERFETTO MACRO _critical_path(
        graph_table TableOrSubquery, root_table TableOrSubquery, sleeping_table TableOrSubquery)
RETURNS TableOrSubquery
AS (
  WITH
    critical_path AS (
      SELECT * FROM graph_reachable_weight_bounded_dfs !($graph_table, $root_table, 1)
    )
  SELECT
    ts,
    dur,
    root_id,
    id,
    utid,
    critical_path_utid,
    critical_path_id,
    critical_path_blocked_dur,
    critical_path_blocked_state,
    critical_path_blocked_function
  FROM _flatten_critical_paths!(critical_path, $sleeping_table)
  UNION ALL
  -- Add roots
  SELECT
    ts,
    end_ts - ts AS dur,
    id AS root_id,
    id,
    utid,
    utid AS critical_path_utid,
    NULL AS critical_path_id,
    NULL AS critical_path_blocked_dur,
    NULL AS critical_path_blocked_state,
    NULL AS critical_path_blocked_function
  FROM $root_table
  ORDER BY root_id
);

-- Generates the critical path for only the set of roots <id> passed in.
-- _intervals_to_roots can be used to generate root ids from a given time interval.
-- This can be used to genrate the critical path over sparse regions of a trace, e.g
-- binder transactions. It might be more efficient to generate the _critical_path
-- for the entire trace, see _thread_executing_span_critical_path_all, but for a
-- per-process susbset of binder txns for instance, this is likely faster.
CREATE PERFETTO MACRO _critical_path_by_roots(roots_table TableOrSubQuery)
RETURNS TableOrSubQuery
AS (
  WITH roots AS (
    SELECT * FROM $roots_table
  ), root_bounds AS (
    SELECT MIN(id) AS min_root_id, MAX(id) AS max_root_id FROM roots
  ), wakeup_bounds AS (
    SELECT COALESCE(_wakeup_graph.prev_id, min_root_id) AS min_wakeup, max_root_id AS max_wakeup
    FROM root_bounds
    JOIN _wakeup_graph ON id = min_root_id
  ) SELECT
      id,
      ts,
      dur,
      utid,
      critical_path_id,
      critical_path_blocked_dur,
      critical_path_blocked_state,
      critical_path_blocked_function,
      critical_path_utid
      FROM
        _critical_path
        !(
          (
            SELECT
              id AS source_node_id,
              COALESCE(waker_id, id) AS dest_node_id,
              id - COALESCE(waker_id, id) AS edge_weight
            FROM _wakeup_graph
            JOIN wakeup_bounds WHERE id BETWEEN min_wakeup AND max_wakeup
          ),
          (
            SELECT
              _wakeup_graph.id AS root_node_id,
              _wakeup_graph.id - COALESCE(prev_id, _wakeup_graph.id) AS root_target_weight,
              id,
              ts,
              end_ts,
              utid
            FROM _wakeup_graph
            JOIN (SELECT * FROM roots) USING (id)
          ),
          _sleep));

-- Generates the critical path for only the time intervals for the utids given.
-- Currently expensive because of naive interval_intersect implementation.
-- Prefer _critical_paths_by_roots for performance. This is useful for a small
-- set of intervals, e.g app startups in a trace.
CREATE PERFETTO MACRO _critical_path_by_intervals(intervals_table TableOrSubQuery)
RETURNS TableOrSubQuery AS (
WITH span_starts AS (
    SELECT
      id,
      MAX(span.ts, intervals.ts) AS ts,
      MIN(span.ts + span.dur, intervals.ts + intervals.dur) AS end_ts,
      span.utid,
      critical_path_id,
      critical_path_blocked_dur,
      critical_path_blocked_state,
      critical_path_blocked_function,
      critical_path_utid
    FROM _critical_path_by_roots!(_intervals_to_roots!($intervals_table)) span
    -- TODO(zezeozue): Replace with interval_intersect when partitions are supported
    JOIN (SELECT * FROM $intervals_table) intervals ON span.critical_path_utid = intervals.utid
        AND ((span.ts BETWEEN intervals.ts AND intervals.ts + intervals.dur)
             OR (intervals.ts BETWEEN span.ts AND span.ts + span.dur))
) SELECT
      id,
      ts,
      end_ts - ts AS dur,
      utid,
      critical_path_id,
      critical_path_blocked_dur,
      critical_path_blocked_state,
      critical_path_blocked_function,
      critical_path_utid
   FROM span_starts);

-- Generates the critical path for a given utid over the <ts, dur> interval.
-- The duration of a thread executing span in the critical path is the range between the
-- start of the thread_executing_span and the start of the next span in the critical path.
CREATE PERFETTO FUNCTION _thread_executing_span_critical_path(
  -- Utid of the thread to compute the critical path for.
  critical_path_utid INT,
  -- Timestamp.
  ts LONG,
  -- Duration.
  dur LONG)
RETURNS TABLE(
  -- Id of the first (runnable) thread state in thread_executing_span.
  id INT,
  -- Timestamp of first thread_state in thread_executing_span.
  ts LONG,
  -- Duration of thread_executing_span.
  dur LONG,
  -- Utid of thread with thread_state.
  utid INT,
  -- Id of thread executing span following the sleeping thread state for which the critical path is computed.
  critical_path_id INT,
  -- Critical path duration.
  critical_path_blocked_dur LONG,
  -- Sleeping thread state in critical path.
  critical_path_blocked_state STRING,
  -- Kernel blocked_function of the critical path.
  critical_path_blocked_function STRING,
  -- Thread Utid the critical path was filtered to.
  critical_path_utid INT
) AS
SELECT * FROM _critical_path_by_intervals!((SELECT $critical_path_utid AS utid, $ts as ts, $dur AS dur));

-- Generates the critical path for all threads for the entire trace duration.
-- The duration of a thread executing span in the critical path is the range between the
-- start of the thread_executing_span and the start of the next span in the critical path.
CREATE PERFETTO FUNCTION _thread_executing_span_critical_path_all()
RETURNS
  TABLE(
    -- Id of the first (runnable) thread state in thread_executing_span.
    id INT,
    -- Timestamp of first thread_state in thread_executing_span.
    ts LONG,
    -- Duration of thread_executing_span.
    dur LONG,
    -- Utid of thread with thread_state.
    utid INT,
    -- Id of thread executing span following the sleeping thread state for which the critical path is computed.
    critical_path_id INT,
    -- Critical path duration.
    critical_path_blocked_dur LONG,
    -- Sleeping thread state in critical path.
    critical_path_blocked_state STRING,
    -- Kernel blocked_function of the critical path.
    critical_path_blocked_function STRING,
    -- Thread Utid the critical path was filtered to.
    critical_path_utid INT)
AS
SELECT
  id,
  ts,
  dur,
  utid,
  critical_path_id,
  critical_path_blocked_dur,
  critical_path_blocked_state,
  critical_path_blocked_function,
  critical_path_utid
FROM _critical_path_by_roots!((SELECT id FROM _wakeup_graph));
