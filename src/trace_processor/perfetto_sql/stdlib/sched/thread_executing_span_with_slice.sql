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
--

INCLUDE PERFETTO MODULE slices.flat_slices;
INCLUDE PERFETTO MODULE sched.thread_executing_span;

CREATE PERFETTO TABLE _critical_path_all AS
SELECT * FROM  _thread_executing_span_critical_path_all();

-- Limited thread_state view that will later be span joined with the |_thread_executing_span_graph|.
CREATE PERFETTO VIEW _span_thread_state_view
AS SELECT id AS thread_state_id, ts, dur, utid, state, blocked_function as function, io_wait, cpu FROM thread_state;

-- Limited slice_view that will later be span joined with the |_thread_executing_span_graph|.
CREATE PERFETTO VIEW _span_slice_view
AS
SELECT
  slice_id,
  depth AS slice_depth,
  name AS slice_name,
  CAST(ts AS INT) AS ts,
  CAST(dur AS INT) AS dur,
  utid
FROM _slice_flattened;

CREATE VIRTUAL TABLE _span_thread_state_slice_view
USING
  SPAN_LEFT_JOIN(
    _span_thread_state_view PARTITIONED utid,
    _span_slice_view PARTITIONED utid);

-- |_thread_executing_span_graph| span joined with thread_state information.
CREATE VIRTUAL TABLE _span_critical_path_thread_state_slice_sp
USING
  SPAN_JOIN(
    _critical_path_all PARTITIONED utid,
    _span_thread_state_slice_view PARTITIONED utid);

-- |_thread_executing_span_graph| + thread_state view joined with critical_path information.
CREATE PERFETTO TABLE _critical_path_thread_state_slice AS
WITH span_starts AS (
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
      thread_state.io_wait,
      thread_state.slice_id,
      thread_state.slice_name,
      thread_state.slice_depth
    FROM _critical_path_all span
    JOIN _span_critical_path_thread_state_slice_sp thread_state USING(id)
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
  slice_id,
  slice_name,
  slice_depth,
  critical_path_id,
  critical_path_blocked_dur,
  critical_path_blocked_state,
  critical_path_blocked_function,
  critical_path_utid
FROM span_starts
WHERE MIN(span_end_ts, thread_state_end_ts) - ts > 0;

-- Flattened slices span joined with their thread_states. This contains the 'self' information
-- without 'critical_path' (blocking) information.
CREATE VIRTUAL TABLE _self_sp USING
  SPAN_LEFT_JOIN(thread_state PARTITIONED utid, _slice_flattened PARTITIONED utid);

-- Limited view of |_self_sp|.
CREATE PERFETTO VIEW _self_view
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
    FROM _self_sp;

-- Self and critical path span join. This contains the union of the time intervals from the following:
--  a. Self slice stack + thread_state.
--  b. Critical path stack + thread_state.
CREATE VIRTUAL TABLE _self_and_critical_path_sp
USING
  SPAN_JOIN(
    _self_view PARTITIONED critical_path_utid,
    _critical_path_thread_state_slice PARTITIONED critical_path_utid);

-- Returns a view of |_self_and_critical_path_sp| unpivoted over the following columns:
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
CREATE PERFETTO FUNCTION _critical_path_stack(critical_path_utid INT, ts LONG, dur LONG, enable_process_name INT, enable_thread_name INT, enable_self_slice INT, enable_critical_path_slice INT)
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
    FROM _self_and_critical_path_sp
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
      critical_path_utid AS utid,
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
      critical_path_utid AS utid,
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
      critical_path_utid AS utid,
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
      critical_path_utid AS utid,
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
      critical_path_utid AS utid,
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
CREATE PERFETTO FUNCTION _thread_executing_span_critical_path_stack(
  -- Thread utid to filter critical paths to.
  critical_path_utid INT,
  -- Timestamp of start of time range to filter critical paths to.
  ts LONG,
  -- Duration of time range to filter critical paths to.
  dur LONG)
RETURNS
  TABLE(
    -- Id of the thread_state or slice in the thread_executing_span.
    id INT,
    -- Timestamp of slice in the critical path.
    ts LONG,
    -- Duration of slice in the critical path.
    dur LONG,
    -- Utid of thread that emitted the slice.
    utid INT,
    -- Stack depth of the entitity in the debug track.
    stack_depth INT,
    -- Name of entity in the critical path (could be a thread_state, kernel blocked_function, process_name, thread_name, slice name or cpu).
    name STRING,
    -- Table name of entity in the critical path (could be either slice or thread_state).
    table_name STRING,
    -- Utid of the thread the critical path was filtered to.
    critical_path_utid INT
) AS
SELECT * FROM _critical_path_stack($critical_path_utid, $ts, $dur, 1, 1, 1, 1);

-- Returns a pprof aggregation of the stacks in |_critical_path_stack|.
CREATE PERFETTO FUNCTION _critical_path_graph(graph_title STRING, critical_path_utid INT, ts LONG, dur LONG, enable_process_name INT, enable_thread_name INT, enable_self_slice INT, enable_critical_path_slice INT)
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
      _critical_path_stack($critical_path_utid, $ts, $dur, $enable_process_name, $enable_thread_name, $enable_self_slice, $enable_critical_path_slice)
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

-- Returns a pprof aggreagation of the stacks in |_thread_executing_span_critical_path_stack|
CREATE PERFETTO FUNCTION _thread_executing_span_critical_path_graph(
  -- Descriptive name for the graph.
  graph_title STRING,
  -- Thread utid to filter critical paths to.
  critical_path_utid INT,
  -- Timestamp of start of time range to filter critical paths to.
  ts INT,
  -- Duration of time range to filter critical paths to.
  dur INT)
RETURNS TABLE(
  -- Pprof of critical path stacks.
  pprof BYTES
)
AS
SELECT * FROM _critical_path_graph($graph_title, $critical_path_utid, $ts, $dur, 1, 1, 1, 1);
