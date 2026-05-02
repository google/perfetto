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

-- Critical-path stack and pprof aggregation, scoped to a
-- `(root_utid, ts, dur)` window. The intermediates are built lazily
-- inside the function bodies (the walk only visits wakeup nodes that
-- overlap the window, and the SPAN_LEFT_JOINs below are queried with
-- `utid` push-down) so module load stays cheap.

-- Projections fed into the SPAN_LEFT_JOIN virtual tables. No rows
-- are produced until a function body queries them with a `utid`
-- filter.
CREATE PERFETTO VIEW _span_thread_state_view AS
SELECT
  id AS thread_state_id,
  ts,
  dur,
  utid,
  state,
  blocked_function AS function,
  io_wait,
  cpu
FROM thread_state;

CREATE PERFETTO VIEW _span_slice_view AS
SELECT
  slice_id,
  depth AS slice_depth,
  cast_int!(ts) AS ts,
  cast_int!(dur) AS dur,
  utid
FROM _slice_flattened;

-- thread_state × flat-slice for the blocker side of the critical
-- path. Function bodies filter to the utids the per-window CP touches.
CREATE VIRTUAL TABLE _span_thread_state_slice_sp USING SPAN_LEFT_JOIN (
    _span_thread_state_view PARTITIONED utid,
    _span_slice_view PARTITIONED utid);

-- thread_state × flat-slice for the self side. Function bodies
-- filter to `utid = $root_utid`.
CREATE VIRTUAL TABLE _self_sp USING SPAN_LEFT_JOIN (thread_state PARTITIONED utid, _slice_flattened PARTITIONED utid);

-- Self-side and blocker-side spans for `(root_utid, ts, dur)`,
-- intersected and clipped to the window. Pulled out as its own
-- function so callers reference it once via `MATERIALIZED` instead of
-- evaluating the body per UNION-ALL branch.
CREATE PERFETTO FUNCTION _critical_path_relevant_spans(
    root_utid JOINID(thread.id),
    ts TIMESTAMP,
    dur DURATION
)
RETURNS TABLE (
  self_thread_state_id LONG,
  self_state STRING,
  self_slice_id LONG,
  self_slice_depth LONG,
  self_function STRING,
  self_io_wait LONG,
  thread_state_id LONG,
  state STRING,
  function STRING,
  io_wait LONG,
  slice_id LONG,
  slice_depth LONG,
  cpu LONG,
  utid JOINID(thread.id),
  ts TIMESTAMP,
  dur DURATION,
  root_utid JOINID(thread.id)
) AS
WITH
  -- Per-root critical-path frames intersected with the query window.
  _scoped_cp AS MATERIALIZED (
    SELECT
      cr.ts,
      cr.dur,
      g.utid
    FROM _critical_path_by_roots!(
      _intervals_to_roots!(
        (SELECT $root_utid AS utid, $ts AS ts, $dur AS dur),
        _wakeup_graph),
      _wakeup_graph) AS cr
    JOIN _wakeup_graph AS g
      ON g.id = cr.id
    WHERE
      cr.dur > 0 AND cr.ts < $ts + $dur AND cr.ts + cr.dur > $ts
  ),
  -- Blocker thread_state × flat-slice for the utids the CP touches.
  -- `utid IN (subquery)` is required for SPAN_LEFT_JOIN partition
  -- push-down; a `JOIN ... USING (utid)` scans all partitions and is
  -- ~4x slower.
  _scoped_blocker_th_slice AS MATERIALIZED (
    SELECT
      sp.thread_state_id,
      sp.ts,
      sp.dur,
      sp.utid,
      sp.state,
      sp.function,
      sp.cpu,
      sp.io_wait,
      sp.slice_id,
      sp.slice_depth
    FROM _span_thread_state_slice_sp AS sp
    WHERE
      sp.utid IN (
        SELECT DISTINCT
          utid
        FROM _scoped_cp
      )
      AND sp.dur > 0
      AND sp.ts < $ts + $dur
      AND sp.ts + sp.dur > $ts
  ),
  -- CP x blocker thread_state-slice intersection. Open-coded with
  -- overlap predicates rather than `_interval_intersect!`: the macro's
  -- twin evaluation of the inputs interacts badly with the join-back
  -- needed to recover the slice columns and is ~40x slower here even
  -- with MATERIALIZED inputs.
  _scoped_cp_th_slice AS MATERIALIZED (
    SELECT
      max(cp.ts, bts.ts) AS ts,
      min(cp.ts + cp.dur, bts.ts + bts.dur) - max(cp.ts, bts.ts) AS dur,
      bts.thread_state_id,
      bts.utid,
      bts.state,
      bts.function,
      bts.cpu,
      bts.io_wait,
      bts.slice_id,
      bts.slice_depth
    FROM _scoped_cp AS cp
    JOIN _scoped_blocker_th_slice AS bts
      ON bts.utid = cp.utid AND bts.ts < cp.ts + cp.dur AND bts.ts + bts.dur > cp.ts
  ),
  -- Self thread_state x flat-slice for `$root_utid` in the window.
  -- SPAN_LEFT_JOIN keeps thread_state rows even when no slice
  -- overlaps.
  _scoped_self AS MATERIALIZED (
    SELECT
      sp.id AS self_thread_state_id,
      sp.slice_id AS self_slice_id,
      sp.ts,
      sp.dur,
      sp.state AS self_state,
      sp.blocked_function AS self_function,
      sp.cpu AS self_cpu,
      sp.io_wait AS self_io_wait,
      sp.depth AS self_slice_depth
    FROM _self_sp AS sp
    WHERE
      sp.utid = $root_utid AND sp.dur > 0 AND sp.ts < $ts + $dur AND sp.ts + sp.dur > $ts
  )
-- Interval intersection of self x CP. Both sides are already pinned
-- to `root_utid = $root_utid`; output ts/dur are clipped to the query
-- window.
SELECT
  self.self_thread_state_id,
  self.self_state,
  self.self_slice_id,
  self.self_slice_depth,
  self.self_function,
  self.self_io_wait,
  cps.thread_state_id,
  cps.state,
  cps.function,
  cps.io_wait,
  cps.slice_id,
  cps.slice_depth,
  cps.cpu,
  cps.utid,
  max(self.ts, cps.ts, $ts) AS ts,
  min(self.ts + self.dur, cps.ts + cps.dur, $ts + $dur) - max(self.ts, cps.ts, $ts) AS dur,
  $root_utid AS root_utid
FROM _scoped_self AS self
JOIN _scoped_cp_th_slice AS cps
  ON cps.ts < self.ts + self.dur AND cps.ts + cps.dur > self.ts
WHERE
  min(self.ts + self.dur, cps.ts + cps.dur, $ts + $dur) > max(self.ts, cps.ts, $ts);

-- Per-(root_utid, ts) stack of unpivoted rows describing the critical
-- path at each timestamp:
--   * self thread_state
--   * self blocked_function (when blocked)
--   * self process_name (when |enable_process_name|)
--   * self thread_name (when |enable_thread_name|)
--   * self slice stack (when |enable_self_slice|)
--   * critical-path thread_state
--   * critical-path process_name / thread_name
--   * critical-path slice stack (when |enable_critical_path_slice|)
--   * running cpu (when on-CPU)
-- A 'stack' is the group of rows sharing the same `ts`.
CREATE PERFETTO FUNCTION _critical_path_stack(
    root_utid JOINID(thread.id),
    ts TIMESTAMP,
    dur DURATION,
    enable_process_name LONG,
    enable_thread_name LONG,
    enable_self_slice LONG,
    enable_critical_path_slice LONG
)
RETURNS TABLE (
  id LONG,
  ts TIMESTAMP,
  dur DURATION,
  utid JOINID(thread.id),
  stack_depth LONG,
  name STRING,
  table_name STRING,
  root_utid JOINID(thread.id)
) AS
WITH
  -- Materialise the helper once; the UNION ALLs below reference it
  -- ten times.
  relevant_spans AS MATERIALIZED (
    SELECT
      *
    FROM _critical_path_relevant_spans($root_utid, $ts, $dur)
    WHERE
      dur > 0
  ),
  -- 1. Builds the 'self' stack of items as an ordered UNION ALL
  self_stack AS MATERIALIZED (
    -- Builds the self thread_state
    SELECT
      self_thread_state_id AS id,
      ts,
      dur,
      root_utid AS utid,
      0 AS stack_depth,
      'thread_state: ' || self_state AS name,
      'thread_state' AS table_name,
      root_utid
    FROM relevant_spans
    UNION ALL
    -- Builds the self kernel blocked_function
    SELECT
      self_thread_state_id AS id,
      ts,
      dur,
      root_utid AS utid,
      1 AS stack_depth,
      iif(self_state GLOB 'R*', NULL, 'kernel function: ' || self_function) AS name,
      'thread_state' AS table_name,
      root_utid
    FROM relevant_spans
    UNION ALL
    -- Builds the self kernel io_wait
    SELECT
      self_thread_state_id AS id,
      ts,
      dur,
      root_utid AS utid,
      2 AS stack_depth,
      iif(self_state GLOB 'R*', NULL, 'io_wait: ' || self_io_wait) AS name,
      'thread_state' AS table_name,
      root_utid
    FROM relevant_spans
    UNION ALL
    -- Builds the self process_name
    SELECT
      self_thread_state_id AS id,
      ts,
      dur,
      thread.utid,
      3 AS stack_depth,
      iif($enable_process_name, 'process_name: ' || process.name, NULL) AS name,
      'thread_state' AS table_name,
      root_utid
    FROM relevant_spans
    LEFT JOIN thread
      ON thread.utid = root_utid
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
      iif($enable_thread_name, 'thread_name: ' || thread.name, NULL) AS name,
      'thread_state' AS table_name,
      root_utid
    FROM relevant_spans
    LEFT JOIN thread
      ON thread.utid = root_utid
    JOIN process
      USING (upid)
    UNION ALL
    -- Builds the self 'ancestor' slice stack
    SELECT
      anc.id,
      slice.ts,
      slice.dur,
      root_utid AS utid,
      anc.depth + 5 AS stack_depth,
      iif($enable_self_slice, anc.name, NULL) AS name,
      'slice' AS table_name,
      root_utid
    FROM relevant_spans AS slice, ancestor_slice(self_slice_id) AS anc
    WHERE
      anc.dur != -1
    UNION ALL
    -- Self 'deepest' slice. The slice name is fetched here on
    -- `self_slice_id` so the upstream materialised tables stay in
    -- id-space.
    SELECT
      spans.self_slice_id AS id,
      spans.ts,
      spans.dur,
      spans.root_utid AS utid,
      spans.self_slice_depth + 5 AS stack_depth,
      iif($enable_self_slice, sl.name, NULL) AS name,
      'slice' AS table_name,
      spans.root_utid
    FROM relevant_spans AS spans
    LEFT JOIN slice AS sl
      ON sl.id = spans.self_slice_id
    ORDER BY
      stack_depth
  ),
  -- Prepares for stage 2 in building the entire stack.
  -- Computes the starting depth for each stack. This is necessary because
  -- each self slice stack has variable depth and the depth in each stack
  -- most be contiguous in order to efficiently generate a pprof in the future.
  critical_path_start_depth AS MATERIALIZED (
    SELECT
      root_utid,
      ts,
      max(stack_depth) + 1 AS start_depth
    FROM self_stack
    GROUP BY
      root_utid,
      ts
  ),
  critical_path_span AS MATERIALIZED (
    SELECT
      thread_state_id,
      state,
      function,
      io_wait,
      slice_id,
      slice_depth,
      spans.ts,
      spans.dur,
      spans.root_utid,
      utid,
      start_depth
    FROM relevant_spans AS spans
    JOIN critical_path_start_depth
      ON critical_path_start_depth.root_utid = spans.root_utid
      AND critical_path_start_depth.ts = spans.ts
    WHERE
      critical_path_start_depth.root_utid = $root_utid
      AND spans.root_utid != spans.utid
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
      root_utid
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
      root_utid
    FROM critical_path_span
    JOIN thread
      USING (utid)
    LEFT JOIN process
      USING (upid)
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
      root_utid
    FROM critical_path_span
    JOIN thread
      USING (utid)
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
      root_utid
    FROM critical_path_span
    JOIN thread
      USING (utid)
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
      root_utid
    FROM critical_path_span
    JOIN thread
      USING (utid)
    UNION ALL
    -- Builds the critical_path 'ancestor' slice stack
    SELECT
      anc.id,
      slice.ts,
      slice.dur,
      slice.utid,
      anc.depth + start_depth + 5 AS stack_depth,
      iif($enable_critical_path_slice, anc.name, NULL) AS name,
      'slice' AS table_name,
      root_utid
    FROM critical_path_span AS slice, ancestor_slice(slice_id) AS anc
    WHERE
      anc.dur != -1
    UNION ALL
    -- Critical-path 'deepest' slice. The slice name is fetched here on
    -- `slice_id` so the upstream materialised tables stay in id-space.
    SELECT
      cps.slice_id AS id,
      cps.ts,
      cps.dur,
      cps.utid,
      cps.slice_depth + cps.start_depth + 5 AS stack_depth,
      iif($enable_critical_path_slice, sl.name, NULL) AS name,
      'slice' AS table_name,
      cps.root_utid
    FROM critical_path_span AS cps
    LEFT JOIN slice AS sl
      ON sl.id = cps.slice_id
    ORDER BY
      stack_depth
  ),
  -- Prepares for stage 3 in building the entire stack.
  -- Computes the starting depth for each stack using the deepest stack_depth between
  -- the critical_path stack and self stack. The self stack depth is
  -- already computed and materialized in |critical_path_start_depth|.
  cpu_start_depth_raw AS (
    SELECT
      root_utid,
      ts,
      max(stack_depth) + 1 AS start_depth
    FROM critical_path_stack
    GROUP BY
      root_utid,
      ts
    UNION ALL
    SELECT
      *
    FROM critical_path_start_depth
  ),
  cpu_start_depth AS (
    SELECT
      root_utid,
      ts,
      max(start_depth) AS start_depth
    FROM cpu_start_depth_raw
    GROUP BY
      root_utid,
      ts
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
      spans.root_utid
    FROM relevant_spans AS spans
    JOIN cpu_start_depth
      ON cpu_start_depth.root_utid = spans.root_utid AND cpu_start_depth.ts = spans.ts
    WHERE
      cpu_start_depth.root_utid = $root_utid
      AND state = 'Running'
      OR self_state = 'Running'
  ),
  merged AS (
    SELECT
      *
    FROM self_stack
    UNION ALL
    SELECT
      *
    FROM critical_path_stack
    UNION ALL
    SELECT
      *
    FROM cpu_stack
  )
SELECT
  *
FROM merged
WHERE
  id IS NOT NULL;

-- Critical path stack of thread_executing_spans with the following entities in the critical path
-- stacked from top to bottom: self thread_state, self blocked_function, self process_name,
-- self thread_name, slice stack, critical_path thread_state, critical_path process_name,
-- critical_path thread_name, critical_path slice_stack, running_cpu.
CREATE PERFETTO FUNCTION _thread_executing_span_critical_path_stack(
    -- Thread utid to filter critical paths to.
    root_utid JOINID(thread.id),
    -- Timestamp of start of time range to filter critical paths to.
    ts TIMESTAMP,
    -- Duration of time range to filter critical paths to.
    dur DURATION
)
RETURNS TABLE (
  -- Id of the thread_state or slice in the thread_executing_span.
  id LONG,
  -- Timestamp of slice in the critical path.
  ts TIMESTAMP,
  -- Duration of slice in the critical path.
  dur DURATION,
  -- Utid of thread that emitted the slice.
  utid JOINID(thread.id),
  -- Stack depth of the entitity in the debug track.
  stack_depth LONG,
  -- Name of entity in the critical path (could be a thread_state, kernel blocked_function, process_name, thread_name, slice name or cpu).
  name STRING,
  -- Table name of entity in the critical path (could be either slice or thread_state).
  table_name STRING,
  -- Utid of the thread the critical path was filtered to.
  root_utid JOINID(thread.id)
) AS
SELECT
  *
FROM _critical_path_stack($root_utid, $ts, $dur, 1, 1, 1, 1);

-- Returns a pprof aggregation of the stacks in |_critical_path_stack|.
CREATE PERFETTO FUNCTION _critical_path_graph(
    graph_title STRING,
    root_utid JOINID(thread.id),
    ts TIMESTAMP,
    dur DURATION,
    enable_process_name LONG,
    enable_thread_name LONG,
    enable_self_slice LONG,
    enable_critical_path_slice LONG
)
RETURNS TABLE (
  pprof BYTES
) AS
WITH
  stack AS MATERIALIZED (
    SELECT
      ts,
      dur - coalesce(lead(dur) OVER (PARTITION BY root_utid, ts ORDER BY stack_depth), 0) AS dur,
      name,
      utid,
      root_utid,
      stack_depth
    FROM _critical_path_stack(
      $root_utid,
      $ts,
      $dur,
      $enable_process_name,
      $enable_thread_name,
      $enable_self_slice,
      $enable_critical_path_slice
    )
  ),
  graph AS (
    SELECT
      cat_stacks($graph_title) AS stack
  ),
  parent AS (
    SELECT
      cr.ts,
      cr.dur,
      cr.name,
      cr.utid,
      cr.stack_depth,
      cat_stacks(graph.stack, cr.name) AS stack,
      cr.root_utid
    FROM stack AS cr, graph
    WHERE
      stack_depth = 0
    UNION ALL
    SELECT
      child.ts,
      child.dur,
      child.name,
      child.utid,
      child.stack_depth,
      cat_stacks(stack, child.name) AS stack,
      child.root_utid
    FROM stack AS child
    JOIN parent
      ON parent.root_utid = child.root_utid
      AND parent.ts = child.ts
      AND child.stack_depth = parent.stack_depth + 1
  ),
  stacks AS (
    SELECT
      dur,
      stack
    FROM parent
  )
SELECT
  experimental_profile(stack, 'duration', 'ns', dur) AS pprof
FROM stacks;

-- Returns a pprof aggreagation of the stacks in |_thread_executing_span_critical_path_stack|
CREATE PERFETTO FUNCTION _thread_executing_span_critical_path_graph(
    -- Descriptive name for the graph.
    graph_title STRING,
    -- Thread utid to filter critical paths to.
    root_utid JOINID(thread.id),
    -- Timestamp of start of time range to filter critical paths to.
    ts TIMESTAMP,
    -- Duration of time range to filter critical paths to.
    dur DURATION
)
RETURNS TABLE (
  -- Pprof of critical path stacks.
  pprof BYTES
) AS
SELECT
  *
FROM _critical_path_graph($graph_title, $root_utid, $ts, $dur, 1, 1, 1, 1);
