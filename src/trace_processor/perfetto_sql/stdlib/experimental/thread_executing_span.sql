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

INCLUDE PERFETTO MODULE common.slices;
INCLUDE PERFETTO MODULE experimental.flat_slices;

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
CREATE VIEW internal_runnable_state
AS
SELECT
  thread_state.id,
  thread_state.ts,
  thread_state.dur,
  thread_state.state,
  thread_state.utid,
  thread_state.waker_utid
FROM thread_state
WHERE thread_state.dur != -1 AND thread_state.waker_utid IS NOT NULL
   AND (thread_state.irq_context = 0 OR thread_state.irq_context IS NULL);

-- Similar to |internal_runnable_state| but finds the first runnable state at thread.
CREATE VIEW internal_first_runnable_state
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
  thread_state.waker_utid
FROM thread_state
JOIN first_state USING (id)
WHERE thread_state.dur != -1 AND thread_state.state = 'R';

--
-- Finds all sleep states including interruptible (S) and uninterruptible (D).
CREATE VIEW internal_sleep_state
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
CREATE VIEW internal_thread_end_ts
AS
SELECT
  MAX(ts) + dur AS end_ts,
  utid
FROM thread_state
WHERE dur != -1
GROUP BY utid;

-- Similar to |internal_sleep_state| but finds the first sleep state in a thread.
CREATE VIEW internal_first_sleep_state
AS
SELECT
  MIN(s.id) AS id,
  s.ts,
  s.dur,
  s.state,
  s.blocked_function,
  s.utid
FROM internal_sleep_state s
JOIN internal_runnable_state r
  ON s.utid = r.utid AND (s.ts + s.dur = r.ts)
GROUP BY s.utid;

--
-- Finds all neighbouring ('Sleeping', 'Runnable') thread_states pairs from the same thread.
-- More succintly, pairs of S[n-1]-R[n] where R is woken by a process context and S is an
-- interruptible or uninterruptible sleep state.
--
-- This is achieved by joining the |internal_runnable_state|.ts with the
-- |internal_sleep_state|.|ts + dur|.
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
-- prev_start_id    = R0_id.
-- prev_start_ts    = R0_ts.
-- prev_start_dur   = R0_dur.
-- prev_start_state = 'R'.
--
-- prev_end_id      = S0_id.
-- prev_end_ts      = S0_ts.
-- prev_end_dur     = S0_dur.
-- prev_end_state   = 'S' or 'D'.
--
-- start_id         = R1_id.
-- start_ts         = R1_ts.
-- start_dur        = R1_dur.
-- start_state      = 'R'.
--
-- end_id           = S1_id.
-- end_ts           = S1_ts.
-- end_dur          = S1_dur.
-- end_state        = 'S' or 'D'.
CREATE TABLE internal_wakeup AS
  SELECT
  LAG(r.id, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS prev_start_id,
  LAG(r.ts, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS prev_start_ts,
  LAG(r.dur, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS prev_start_dur,
  LAG(r.state, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS prev_start_state,
  s.id AS prev_end_id,
  s.ts AS prev_end_ts,
  s.dur AS prev_end_dur,
  s.state AS prev_end_state,
  s.blocked_function AS prev_blocked_function,
  r.id AS start_id,
  r.ts AS start_ts,
  r.dur AS start_dur,
  r.state AS start_state,
  r.utid AS utid,
  r.waker_utid,
  LEAD(s.id, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS end_id,
  IFNULL(LEAD(s.ts, 1) OVER (PARTITION BY r.utid ORDER BY r.ts), thread_end.end_ts)  AS end_ts,
  LEAD(s.dur, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS end_dur,
  LEAD(s.state, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS end_state,
  LEAD(s.blocked_function, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS blocked_function
FROM internal_runnable_state r
JOIN internal_sleep_state s
  ON s.utid = r.utid AND (s.ts + s.dur = r.ts)
LEFT JOIN internal_thread_end_ts thread_end USING(utid)
UNION ALL
  SELECT
  NULL AS prev_start_id,
  NULL AS prev_start_ts,
  NULL AS prev_start_dur,
  NULL AS prev_start_state,
  NULL AS prev_end_id,
  NULL AS prev_end_ts,
  NULL AS prev_end_dur,
  NULL AS prev_end_state,
  NULL AS prev_blocked_function,
  r.id AS start_id,
  r.ts AS start_ts,
  r.dur AS start_dur,
  r.state AS start_state,
  r.utid AS utid,
  r.waker_utid,
  s.id AS end_id,
  IFNULL(s.ts, thread_end.end_ts)  AS end_ts,
  s.dur AS end_dur,
  s.state AS end_state,
  s.blocked_function AS blocked_function
FROM internal_first_runnable_state r
LEFT JOIN internal_first_sleep_state s
  ON s.utid = r.utid
LEFT JOIN internal_thread_end_ts thread_end USING(utid);

-- Improves performance of |internal_wakeup_chain| computation.
CREATE
  INDEX internal_wakeup_idx
ON internal_wakeup(waker_utid, start_ts);

--
-- Builds the parent-child chain from all thread_executing_spans. The parent is the waker and
-- child is the wakee.
--
-- Note that this doesn't include the roots. We'll compute the roots below.
-- This two step process improves performance because it's more efficient to scan
-- parent and find a child between than to scan child and find the parent it lies between.
CREATE VIEW internal_wakeup_chain
AS
SELECT parent.start_id AS parent_id, child.*
FROM internal_wakeup parent
JOIN internal_wakeup child
  ON (
    parent.utid = child.waker_utid
    AND child.start_ts BETWEEN parent.start_ts AND parent.end_ts);

--
-- Finds the roots of the |internal_wakeup_chain|.
CREATE PERFETTO TABLE internal_wakeup_root
AS
WITH
  internal_wakeup_root_id AS (
    SELECT DISTINCT parent_id AS id FROM internal_wakeup_chain
    EXCEPT
    SELECT DISTINCT start_id AS id FROM internal_wakeup_chain
  )
SELECT NULL AS parent_id, internal_wakeup.*
FROM internal_wakeup
JOIN internal_wakeup_root_id
  ON internal_wakeup_root_id.id = internal_wakeup.start_id;

--
-- Finds the leafs of the |internal_wakeup_chain|.
CREATE PERFETTO TABLE internal_wakeup_leaf AS
WITH
  internal_wakeup_leaf_id AS (
    SELECT DISTINCT start_id AS id FROM internal_wakeup_chain
    EXCEPT
    SELECT DISTINCT parent_id AS id FROM internal_wakeup_chain
  )
SELECT internal_wakeup_chain.*
FROM internal_wakeup_chain
JOIN internal_wakeup_leaf_id
  ON internal_wakeup_leaf_id.id = internal_wakeup_chain.start_id;

--
-- Merges the roots, leafs and the rest of the chain.
CREATE TABLE internal_wakeup_graph
AS
SELECT internal_wakeup_chain.*, 0 AS is_root, (internal_wakeup_leaf.start_id IS NOT NULL) AS is_leaf
FROM internal_wakeup_chain
LEFT JOIN internal_wakeup_leaf
  USING (start_id)
UNION ALL
SELECT *, 1 AS is_root, 0 AS is_leaf FROM internal_wakeup_root;

-- Thread_executing_span graph of all wakeups across all processes.
--
-- @column root_id            Id of thread_executing_span that initiated the wakeup of |id|.
-- @column parent_id          Id of thread_executing_span that directly woke |id|.
-- @column id                 Id of the first (runnable) thread state in thread_executing_span.
-- @column ts                 Timestamp of first thread_state in thread_executing_span.
-- @column dur                Duration of thread_executing_span.
-- @column utid               Utid of thread with thread_state.
-- @column waker_utid         Utid of thread that woke the first thread_state in thread_executing_span.
-- @column blocked_dur        Duration of blocking thread state before waking up.
-- @column blocked_state      Thread state ('D' or 'S') of blocked thread_state before waking up.
-- @column blocked_function   Kernel blocked_function of thread state before waking up.
-- @column is_root            Whether the thread_executing_span is a root.
-- @column depth              Tree depth of thread executing span from the root.
CREATE TABLE experimental_thread_executing_span_graph AS
WITH roots AS (
SELECT
  start_id AS root_id,
  parent_id,
  start_id AS id,
  start_ts AS ts,
  end_ts - start_ts AS dur,
  utid,
  waker_utid,
  prev_end_dur AS blocked_dur,
  prev_end_state AS blocked_state,
  prev_blocked_function AS blocked_function,
  1 AS is_root,
  0 AS depth
FROM internal_wakeup_root
), chain AS (
  SELECT * FROM roots
  UNION ALL
  SELECT
    chain.root_id,
    graph.parent_id,
    graph.start_id AS id,
    graph.start_ts AS ts,
    graph.end_ts - graph.start_ts AS dur,
    graph.utid,
    graph.waker_utid,
    graph.prev_end_dur AS blocked_dur,
    graph.prev_end_state AS blocked_state,
    graph.prev_blocked_function AS blocked_function,
    0 AS is_root,
    chain.depth + 1 AS depth
  FROM internal_wakeup_graph graph
  JOIN chain ON chain.id = graph.parent_id
) SELECT chain.*, thread.upid FROM chain LEFT JOIN thread USING(utid);

-- It finds the MAX between the start of the critical span and the start
-- of the blocked region. This ensures that the critical path doesn't overlap
-- the preceding thread_executing_span before the blocked region.
CREATE PERFETTO FUNCTION internal_critical_path_start_ts(ts LONG, leaf_blocked_ts LONG)
RETURNS LONG AS SELECT MAX($ts, IFNULL($leaf_blocked_ts, $ts));

-- See |experimental_thread_executing_span_critical_path|
CREATE PERFETTO TABLE internal_critical_path
AS
WITH chain AS (
  SELECT
    parent_id,
    id,
    ts,
    dur,
    utid,
    id AS critical_path_id,
    ts - blocked_dur AS critical_path_blocked_ts,
    blocked_dur AS critical_path_blocked_dur,
    blocked_state AS critical_path_blocked_state,
    blocked_function AS critical_path_blocked_function,
    utid AS critical_path_utid,
    upid AS critical_path_upid
  FROM experimental_thread_executing_span_graph graph
  UNION ALL
  SELECT
    graph.parent_id,
    graph.id,
    internal_critical_path_start_ts(graph.ts, chain.critical_path_blocked_ts) AS ts,
    MIN(internal_critical_path_start_ts(graph.ts, chain.critical_path_blocked_ts) + graph.dur, chain.ts)
      - internal_critical_path_start_ts(graph.ts, chain.critical_path_blocked_ts) AS dur,
    graph.utid,
    chain.critical_path_id,
    chain.critical_path_blocked_ts,
    chain.critical_path_blocked_dur,
    chain.critical_path_blocked_state,
    chain.critical_path_blocked_function,
    chain.critical_path_utid,
    chain.critical_path_upid
  FROM experimental_thread_executing_span_graph graph
  JOIN chain ON (chain.parent_id = graph.id AND (chain.ts > chain.critical_path_blocked_ts))
) SELECT * FROM chain;

-- Thread executing span critical paths for all threads. For each thread, the critical path of
-- every sleeping thread state is computed and unioned with the thread executing spans on that thread.
-- The duration of a thread executing span in the critical path is the range between the start of the
-- thread_executing_span and the start of the next span in the critical path.
--
-- @column id                               Id of the first (runnable) thread state in thread_executing_span.
-- @column ts                               Timestamp of first thread_state in thread_executing_span.
-- @column dur                              Duration of thread_executing_span.
-- @column utid                             Utid of thread with thread_state.
-- @column critical_path_id                 Id of thread executing span following the sleeping thread state for which the critical path is computed.
-- @column critical_path_blocked_dur        Critical path duration.
-- @column critical_path_blocked_state      Sleeping thread state in critical path.
-- @column critical_path_blocked_function   Kernel blocked_function of the critical path.
-- @column critical_path_utid               Thread Utid the critical path was filtered to.
CREATE PERFETTO FUNCTION experimental_thread_executing_span_critical_path(critical_path_utid INT, ts LONG, dur LONG)
RETURNS TABLE(
  id INT,
  ts LONG,
  dur LONG,
  utid INT,
  critical_path_id INT,
  critical_path_blocked_dur LONG,
  critical_path_blocked_state STRING,
  critical_path_blocked_function STRING,
  critical_path_utid INT
) AS
WITH span_starts AS (
    SELECT
      id,
      MAX(ts, $ts) AS ts,
      MIN(ts + dur, $ts + $dur) AS end_ts,
      utid,
      critical_path_id,
      critical_path_blocked_dur,
      critical_path_blocked_state,
      critical_path_blocked_function,
      critical_path_utid
    FROM internal_critical_path span
    WHERE (($critical_path_utid IS NOT NULL AND span.critical_path_utid = $critical_path_utid) OR ($critical_path_utid IS NULL))
      AND ((ts BETWEEN $ts AND $ts + $dur) OR ($ts BETWEEN ts AND ts + dur))
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
   FROM span_starts;

-- Limited thread_state view that will later be span joined with the |experimental_thread_executing_span_graph|.
CREATE VIEW internal_span_thread_state_view
AS SELECT id AS thread_state_id, ts, dur, utid, state, blocked_function as function, io_wait, cpu FROM thread_state;

-- |experimental_thread_executing_span_graph| span joined with thread_state information.
CREATE VIRTUAL TABLE internal_span_graph_thread_state_sp
USING
  SPAN_JOIN(
    experimental_thread_executing_span_graph PARTITIONED utid,
    internal_span_thread_state_view PARTITIONED utid);

-- Limited slice_view that will later be span joined with the |experimental_thread_executing_span_graph|.
CREATE VIEW internal_span_slice_view
AS
SELECT slice_id, depth AS slice_depth, name AS slice_name, ts, dur, utid
FROM experimental_slice_flattened;

-- |experimental_thread_executing_span_graph| span joined with slice information.
CREATE VIRTUAL TABLE internal_span_graph_slice_sp
USING
  SPAN_JOIN(
    experimental_thread_executing_span_graph PARTITIONED utid,
    internal_span_slice_view PARTITIONED utid);

-- Limited |experimental_thread_executing_span_graph| + thread_state view.
CREATE VIEW internal_span_graph_thread_state
AS
SELECT ts, dur, id, thread_state_id, state, function, io_wait, cpu
FROM internal_span_graph_thread_state_sp;

-- Limited |experimental_thread_executing_span_graph| + slice view.
CREATE VIEW internal_span_graph_slice
AS
SELECT ts, dur, id, slice_id, slice_depth, slice_name
FROM internal_span_graph_slice_sp;

-- |experimental_thread_executing_span_graph| + thread_state view joined with critical_path information.
CREATE PERFETTO TABLE internal_critical_path_thread_state AS
WITH span AS MATERIALIZED (
    SELECT * FROM internal_critical_path
  ),
  span_starts AS (
    SELECT
      span.id,
      span.utid,
      span.critical_path_id,
      span.critical_path_blocked_dur,
      span.critical_path_blocked_state,
      span.critical_path_blocked_function,
      span.critical_path_utid,
      thread_state_id,
      MAX(thread_state.ts, span.ts) AS ts,
      span.ts + span.dur AS span_end_ts,
      thread_state.ts + thread_state.dur AS thread_state_end_ts,
      thread_state.state,
      thread_state.function,
      thread_state.cpu,
      thread_state.io_wait
    FROM span
    JOIN internal_span_graph_thread_state_sp thread_state USING(id)
  )
SELECT
  id,
  thread_state_id,
  ts,
  MIN(span_end_ts, thread_state_end_ts) - ts AS dur,
  utid,
  state,
  function,
  cpu,
  io_wait,
  critical_path_id,
  critical_path_blocked_dur,
  critical_path_blocked_state,
  critical_path_blocked_function,
  critical_path_utid
FROM span_starts
WHERE MIN(span_end_ts, thread_state_end_ts) - ts > 0;

-- |experimental_thread_executing_span_graph| + thread_state + critical_path span joined with
-- |experimental_thread_executing_span_graph| + slice view.
CREATE VIRTUAL TABLE internal_critical_path_sp
USING
  SPAN_LEFT_JOIN(
    internal_critical_path_thread_state PARTITIONED id,
     internal_span_graph_slice PARTITIONED id);

-- Flattened slices span joined with their thread_states. This contains the 'self' information
-- without 'critical_path' (blocking) information.
CREATE VIRTUAL TABLE internal_self_sp USING
  SPAN_LEFT_JOIN(thread_state PARTITIONED utid, experimental_slice_flattened PARTITIONED utid);

-- Limited view of |internal_self_sp|.
CREATE VIEW internal_self_view
  AS
  SELECT
    id AS self_thread_state_id,
    slice_id AS self_slice_id,
    ts,
    dur,
    utid AS critical_path_utid,
    state AS self_state,
    blocked_function AS self_function,
    cpu AS self_cpu,
    io_wait AS self_io_wait,
    name AS self_slice_name,
    depth AS self_slice_depth
    FROM internal_self_sp;

-- Self and critical path span join. This contains the union of the time intervals from the following:
--  a. Self slice stack + thread_state.
--  b. Critical path stack + thread_state.
CREATE VIRTUAL TABLE internal_self_and_critical_path_sp
USING
  SPAN_JOIN(
    internal_self_view PARTITIONED critical_path_utid,
    internal_critical_path_sp PARTITIONED critical_path_utid);

-- Returns a view of |internal_self_and_critical_path_sp| unpivoted over the following columns:
-- self thread_state.
-- self blocked_function (if one exists).
-- self process_name (enabled with |enable_process_name|).
-- self thread_name (enabled with |enable_thread_name|).
-- self slice_stack (enabled with |enable_self_slice|).
-- critical_path thread_state.
-- critical_path process_name.
-- critical_path thread_name.
-- critical_path slice_stack (enabled with |enable_critical_path_slice|).
-- running cpu (if one exists).
-- A 'stack' is the group of resulting unpivoted rows sharing the same timestamp.
CREATE PERFETTO FUNCTION internal_critical_path_stack(critical_path_utid INT, ts LONG, dur LONG, enable_process_name INT, enable_thread_name INT, enable_self_slice INT, enable_critical_path_slice INT)
RETURNS
  TABLE(
    id INT,
    ts LONG,
    dur LONG,
    utid INT,
    stack_depth INT,
    name STRING,
    table_name STRING,
    critical_path_utid INT) AS
  -- Spans filtered to the query time window and critical_path_utid.
  -- This is a preliminary step that gets the start and end ts of all the rows
  -- so that we can chop the ends of each interval correctly if it overlaps with the query time interval.
  WITH relevant_spans_starts AS (
    SELECT
      self_thread_state_id,
      self_state,
      self_slice_id,
      self_slice_name,
      self_slice_depth,
      self_function,
      self_io_wait,
      thread_state_id,
      state,
      function,
      io_wait,
      slice_id,
      slice_name,
      slice_depth,
      cpu,
      utid,
      MAX(ts, $ts) AS ts,
      MIN(ts + dur, $ts + $dur) AS end_ts,
      critical_path_utid
    FROM internal_self_and_critical_path_sp
    WHERE dur > 0 AND critical_path_utid = $critical_path_utid
  ),
  -- This is the final step that gets the |dur| of each span from the start and
  -- and end ts of the previous step.
  -- Now we manually unpivot the result with 3 key steps: 1) Self 2) Critical path 3) CPU
  -- This CTE is heavily used throughout the entire function so materializing it is
  -- very important.
  relevant_spans AS MATERIALIZED (
    SELECT
      self_thread_state_id,
      self_state,
      self_slice_id,
      self_slice_name,
      self_slice_depth,
      self_function,
      self_io_wait,
      thread_state_id,
      state,
      function,
      io_wait,
      slice_id,
      slice_name,
      slice_depth,
      cpu,
      utid,
      ts,
      end_ts - ts AS dur,
      critical_path_utid,
      utid
    FROM relevant_spans_starts
    WHERE dur > 0
  ),
  -- 1. Builds the 'self' stack of items as an ordered UNION ALL
  self_stack AS MATERIALIZED (
    -- Builds the self thread_state
    SELECT
      self_thread_state_id AS id,
      ts,
      dur,
      utid,
      0 AS stack_depth,
      'thread_state: ' || self_state AS name,
      'thread_state' AS table_name,
      critical_path_utid
    FROM relevant_spans
    UNION ALL
    -- Builds the self kernel blocked_function
    SELECT
      self_thread_state_id AS id,
      ts,
      dur,
      utid,
      1 AS stack_depth,
      IIF(self_state GLOB 'R*', NULL, 'kernel function: ' || self_function) AS name,
      'thread_state' AS table_name,
      critical_path_utid
    FROM relevant_spans
    UNION ALL
    -- Builds the self kernel io_wait
    SELECT
      self_thread_state_id AS id,
      ts,
      dur,
      utid,
      2 AS stack_depth,
      IIF(self_state GLOB 'R*', NULL, 'io_wait: ' || self_io_wait) AS name,
      'thread_state' AS table_name,
      critical_path_utid
    FROM relevant_spans
    UNION ALL
    -- Builds the self process_name
    SELECT
      self_thread_state_id AS id,
      ts,
      dur,
      thread.utid,
      3 AS stack_depth,
      IIF($enable_process_name, 'process_name: ' || process.name, NULL) AS name,
      'thread_state' AS table_name,
      critical_path_utid
    FROM relevant_spans
    LEFT JOIN thread
      ON thread.utid = critical_path_utid
    LEFT JOIN process
      USING (upid)
    -- Builds the self thread_name
    UNION ALL
    SELECT
      self_thread_state_id AS id,
      ts,
      dur,
      thread.utid,
      4 AS stack_depth,
      IIF($enable_thread_name, 'thread_name: ' || thread.name, NULL) AS name,
      'thread_state' AS table_name,
      critical_path_utid
    FROM relevant_spans
    LEFT JOIN thread
      ON thread.utid = critical_path_utid
    JOIN process
      USING (upid)
    UNION ALL
    -- Builds the self 'ancestor' slice stack
    SELECT
      anc.id,
      slice.ts,
      slice.dur,
      slice.utid,
      anc.depth + 5 AS stack_depth,
      IIF($enable_self_slice, anc.name, NULL) AS name,
      'slice' AS table_name,
      critical_path_utid
    FROM relevant_spans slice
    JOIN ancestor_slice(self_slice_id) anc WHERE anc.dur != -1
    UNION ALL
    -- Builds the self 'deepest' ancestor slice stack
    SELECT
      self_slice_id AS id,
      ts,
      dur,
      utid,
      self_slice_depth + 5 AS stack_depth,
      IIF($enable_self_slice, self_slice_name, NULL) AS name,
      'slice' AS table_name,
      critical_path_utid
    FROM relevant_spans slice
    -- Ordering by stack depth is important to ensure the items can
    -- be renedered in the UI as a debug track in the order in which
    -- the sub-queries were 'unioned'.
    ORDER BY stack_depth
  ),
  -- Prepares for stage 2 in building the entire stack.
  -- Computes the starting depth for each stack. This is necessary because
  -- each self slice stack has variable depth and the depth in each stack
  -- most be contiguous in order to efficiently generate a pprof in the future.
  critical_path_start_depth AS MATERIALIZED (
    SELECT critical_path_utid, ts, MAX(stack_depth) + 1 AS start_depth
    FROM self_stack
    GROUP BY critical_path_utid, ts
  ),
  critical_path_span AS MATERIALIZED (
    SELECT
      thread_state_id,
      state,
      function,
      io_wait,
      slice_id,
      slice_name,
      slice_depth,
      spans.ts,
      spans.dur,
      spans.critical_path_utid,
      utid,
      start_depth
    FROM relevant_spans spans
    JOIN critical_path_start_depth
      ON
        critical_path_start_depth.critical_path_utid = spans.critical_path_utid
        AND critical_path_start_depth.ts = spans.ts
    WHERE critical_path_start_depth.critical_path_utid = $critical_path_utid AND spans.critical_path_utid != spans.utid
  ),
  -- 2. Builds the 'critical_path' stack of items as an ordered UNION ALL
  critical_path_stack AS MATERIALIZED (
    -- Builds the critical_path thread_state
    SELECT
      thread_state_id AS id,
      ts,
      dur,
      utid,
      start_depth AS stack_depth,
      'blocking thread_state: ' || state AS name,
      'thread_state' AS table_name,
      critical_path_utid
    FROM critical_path_span
    UNION ALL
    -- Builds the critical_path process_name
    SELECT
      thread_state_id AS id,
      ts,
      dur,
      thread.utid,
      start_depth + 1 AS stack_depth,
      'blocking process_name: ' || process.name,
      'thread_state' AS table_name,
      critical_path_utid
    FROM critical_path_span
    JOIN thread USING (utid)
    LEFT JOIN process USING (upid)
    UNION ALL
    -- Builds the critical_path thread_name
    SELECT
      thread_state_id AS id,
      ts,
      dur,
      thread.utid,
      start_depth + 2 AS stack_depth,
      'blocking thread_name: ' || thread.name,
      'thread_state' AS table_name,
      critical_path_utid
    FROM critical_path_span
    JOIN thread USING (utid)
    UNION ALL
    -- Builds the critical_path kernel blocked_function
    SELECT
      thread_state_id AS id,
      ts,
      dur,
      thread.utid,
      start_depth + 3 AS stack_depth,
      'blocking kernel_function: ' || function,
      'thread_state' AS table_name,
      critical_path_utid
    FROM critical_path_span
    JOIN thread USING (utid)
    UNION ALL
    -- Builds the critical_path kernel io_wait
    SELECT
      thread_state_id AS id,
      ts,
      dur,
      thread.utid,
      start_depth + 4 AS stack_depth,
      'blocking io_wait: ' || io_wait,
      'thread_state' AS table_name,
      critical_path_utid
    FROM critical_path_span
    JOIN thread USING (utid)
    UNION ALL
    -- Builds the critical_path 'ancestor' slice stack
    SELECT
      anc.id,
      slice.ts,
      slice.dur,
      slice.utid,
      anc.depth + start_depth + 5 AS stack_depth,
      IIF($enable_critical_path_slice, anc.name, NULL) AS name,
      'slice' AS table_name,
      critical_path_utid
    FROM critical_path_span slice
    JOIN ancestor_slice(slice_id) anc WHERE anc.dur != -1
    UNION ALL
    -- Builds the critical_path 'deepest' slice
    SELECT
      slice_id AS id,
      ts,
      dur,
      utid,
      slice_depth + start_depth + 5 AS stack_depth,
      IIF($enable_critical_path_slice, slice_name, NULL) AS name,
      'slice' AS table_name,
      critical_path_utid
    FROM critical_path_span slice
    -- Ordering is also important as in the 'self' step above.
    ORDER BY stack_depth
  ),
  -- Prepares for stage 3 in building the entire stack.
  -- Computes the starting depth for each stack using the deepest stack_depth between
  -- the critical_path stack and self stack. The self stack depth is
  -- already computed and materialized in |critical_path_start_depth|.
  cpu_start_depth_raw AS (
    SELECT critical_path_utid, ts, MAX(stack_depth) + 1 AS start_depth
    FROM critical_path_stack
    GROUP BY critical_path_utid, ts
    UNION ALL
    SELECT * FROM critical_path_start_depth
  ),
  cpu_start_depth AS (
    SELECT critical_path_utid, ts, MAX(start_depth) AS start_depth
    FROM cpu_start_depth_raw
    GROUP BY critical_path_utid, ts
  ),
  -- 3. Builds the 'CPU' stack for 'Running' states in either the self or critical path stack.
  cpu_stack AS (
    SELECT
      thread_state_id AS id,
      spans.ts,
      spans.dur,
      utid,
      start_depth AS stack_depth,
      'cpu: ' || cpu AS name,
      'thread_state' AS table_name,
      spans.critical_path_utid
    FROM relevant_spans spans
    JOIN cpu_start_depth
      ON
        cpu_start_depth.critical_path_utid = spans.critical_path_utid
        AND cpu_start_depth.ts = spans.ts
    WHERE cpu_start_depth.critical_path_utid = $critical_path_utid AND state = 'Running' OR self_state = 'Running'
  ),
  merged AS (
    SELECT * FROM self_stack
    UNION ALL
    SELECT * FROM critical_path_stack
    UNION ALL
    SELECT * FROM cpu_stack
  )
SELECT * FROM merged WHERE id IS NOT NULL;

-- Critical path stack of thread_executing_spans with the following entities in the critical path
-- stacked from top to bottom: self thread_state, self blocked_function, self process_name,
-- self thread_name, slice stack, critical_path thread_state, critical_path process_name,
-- critical_path thread_name, critical_path slice_stack, running_cpu.
--
-- @arg critical_path_utid INT    Thread utid to filter critical paths to.
-- @arg ts LONG                   Timestamp of start of time range to filter critical paths to.
-- @arg dur LONG                  Duration of time range to filter critical paths to.
--
-- @column id                     Id of the thread_state or slice in the thread_executing_span.
-- @column ts                     Timestamp of slice in the critical path.
-- @column dur                    Duration of slice in the critical path.
-- @column utid                   Utid of thread that emitted the slice.
-- @column stack_depth            Stack depth of the entitity in the debug track.
-- @column name                   Name of entity in the critical path (could be a thread_state, kernel blocked_function, process_name, thread_name, slice name or cpu).
-- @column table_name             Table name of entity in the critical path (could be either slice or thread_state).
-- @column critical_path_utid     Thread Utid the critical path was filtered to.
CREATE PERFETTO FUNCTION experimental_thread_executing_span_critical_path_stack(critical_path_utid INT, ts LONG, dur LONG)
RETURNS
  TABLE(
    id INT,
    ts LONG,
    dur LONG,
    utid INT,
    stack_depth INT,
    name STRING,
    table_name STRING,
    critical_path_utid INT) AS
SELECT * FROM internal_critical_path_stack($critical_path_utid, $ts, $dur, 1, 1, 1, 1);

-- Returns a pprof aggregation of the stacks in |internal_critical_path_stack|.
CREATE PERFETTO FUNCTION internal_critical_path_graph(graph_title STRING, critical_path_utid INT, ts LONG, dur LONG, enable_process_name INT, enable_thread_name INT, enable_self_slice INT, enable_critical_path_slice INT)
RETURNS TABLE(pprof BYTES)
AS
WITH
  stack AS MATERIALIZED (
    SELECT
      ts,
      dur - IFNULL(LEAD(dur) OVER (PARTITION BY critical_path_utid, ts ORDER BY stack_depth), 0) AS dur,
      name,
      utid,
      critical_path_utid,
      stack_depth
    FROM
      internal_critical_path_stack($critical_path_utid, $ts, $dur, $enable_process_name, $enable_thread_name, $enable_self_slice, $enable_critical_path_slice)
  ),
  graph AS (
    SELECT CAT_STACKS($graph_title) AS stack
  ),
  parent AS (
    SELECT
      cr.ts,
      cr.dur,
      cr.name,
      cr.utid,
      cr.stack_depth,
      CAT_STACKS(graph.stack, cr.name) AS stack,
      cr.critical_path_utid
    FROM stack cr, graph
    WHERE stack_depth = 0
    UNION ALL
    SELECT
      child.ts,
      child.dur,
      child.name,
      child.utid,
      child.stack_depth,
      CAT_STACKS(stack, child.name) AS stack,
      child.critical_path_utid
    FROM stack child
    JOIN parent
      ON
        parent.critical_path_utid = child.critical_path_utid
        AND parent.ts = child.ts
        AND child.stack_depth = parent.stack_depth + 1
  ),
  stacks AS (
    SELECT dur, stack FROM parent
  )
SELECT EXPERIMENTAL_PROFILE(stack, 'duration', 'ns', dur) AS pprof FROM stacks;

-- Returns a pprof aggreagation of the stacks in |experimental_thread_executing_span_critical_path_stack|
--
-- @arg graph_title INT           Descriptive name for the graph.
-- @arg critical_path_utid INT    Thread utid to filter critical paths to.
-- @arg ts LONG                   Timestamp of start of time range to filter critical paths to.
-- @arg dur LONG                  Duration of time range to filter critical paths to.
--
-- @column pprof BYTES            Pprof of critical path stacks.
CREATE PERFETTO FUNCTION experimental_thread_executing_span_critical_path_graph(graph_title STRING, critical_path_utid INT, ts INT, dur INT)
RETURNS TABLE(pprof BYTES)
AS
SELECT * FROM internal_critical_path_graph($graph_title, $critical_path_utid, $ts, $dur, 1, 1, 1, 1);
