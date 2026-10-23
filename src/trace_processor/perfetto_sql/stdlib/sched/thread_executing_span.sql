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

INCLUDE PERFETTO MODULE intervals.overlap;

INCLUDE PERFETTO MODULE intervals.intersect;

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

-- Converts a table with <ts, dur, utid> columns to a unique set of wakeup roots <id> that
-- completely cover the time intervals.
CREATE PERFETTO MACRO _intervals_to_roots(
  _source_table TableOrSubQuery,
  _node_table TableOrSubQuery
)
RETURNS TableOrSubQuery
AS (
  WITH
    _interval_to_root_nodes AS (
      SELECT
        *
      FROM $_node_table
    ),
    _source AS (
      SELECT
        *
      FROM $_source_table
    ),
    _thread_bounds AS (
      SELECT
        utid,
        min(ts) AS min_start,
        max(ts) AS max_start
      FROM _interval_to_root_nodes
      GROUP BY
        utid
    ),
    _start AS (
      SELECT
        _interval_to_root_nodes.utid,
        max(_interval_to_root_nodes.id) AS _start_id,
        _source.ts,
        _source.dur
      FROM _interval_to_root_nodes
      JOIN _thread_bounds
        USING (utid)
      JOIN _source
        ON _source.utid = _interval_to_root_nodes.utid
        AND max(_source.ts, min_start) >= _interval_to_root_nodes.ts
      GROUP BY
        _source.ts,
        _source.utid
    ),
    _end AS (
      SELECT
        _interval_to_root_nodes.utid,
        min(_interval_to_root_nodes.id) AS _end_id,
        _source.ts,
        _source.dur
      FROM _interval_to_root_nodes
      JOIN _thread_bounds
        USING (utid)
      JOIN _source
        ON _source.utid = _interval_to_root_nodes.utid
        AND min((
          _source.ts + _source.dur
        ), max_start) <= _interval_to_root_nodes.ts
      GROUP BY
        _source.ts,
        _source.utid
    ),
    _bound AS (
      SELECT
        _start.utid,
        _start.ts,
        _start.dur,
        _start_id,
        _end_id
      FROM _start
      JOIN _end
        ON _start.ts = _end.ts AND _start.dur = _end.dur AND _start.utid = _end.utid
    )
  SELECT DISTINCT
    id AS root_node_id,
    id - coalesce(prev_id, id) AS capacity
  FROM _bound
  JOIN _interval_to_root_nodes
    ON _interval_to_root_nodes.id BETWEEN _start_id AND _end_id
    AND _interval_to_root_nodes.utid = _bound.utid
);

-- Wakeup graph nodes partitioned into disjoint lanes per (utid, idle_dur IS NULL).
CREATE PERFETTO TABLE _critical_path_node_base AS
SELECT
  id,
  utid,
  ts,
  coalesce(dur, 0) AS dur,
  idle_dur,
  waker_id,
  prev_id,
  (utid << 32)
  | sum(idle_dur IS NULL) OVER (
    PARTITION BY
      utid
    ORDER BY ts, id
    ROWS UNBOUNDED PRECEDING
  ) AS lane
FROM _wakeup_graph
ORDER BY
  id;

-- Earliest waiter timestamp per target lane from hops with known idle_dur.
CREATE PERFETTO TABLE _critical_path_direct_lane_min AS
SELECT w.lane, min(n.ts - n.idle_dur) AS direct_min_ts
FROM _critical_path_node_base AS n
JOIN _critical_path_node_base AS w
  ON w.id = n.waker_id
WHERE
  n.idle_dur IS NOT NULL
  AND min(n.ts, w.ts + w.dur) > n.ts - n.idle_dur
GROUP BY
  w.lane;

-- Propagate earliest waiter timestamp UP across lanes whose first wakeup has idle_dur IS NULL.
CREATE PERFETTO TABLE _critical_path_null_lane_min AS
FROM (
  SELECT
    n.lane AS id,
    iif(
      w.lane != n.lane
      AND w.ts <= n.ts + n.dur
      AND w.lane IN (
        SELECT lane FROM _critical_path_node_base WHERE idle_dur IS NULL
      ),
      w.lane,
      NULL
    ) AS parent_id,
    d.direct_min_ts
  FROM _critical_path_node_base AS n
  LEFT JOIN _critical_path_node_base AS w
    ON w.id = n.waker_id
  LEFT JOIN _critical_path_direct_lane_min AS d
    ON d.lane = n.lane
  WHERE
    n.idle_dur IS NULL
)
|> TREE ACCUMULATE UP MIN(direct_min_ts) AS lane_min_ts
|> SELECT id AS lane, lane_min_ts;

-- Wakeup graph nodes with bounded idle_ts.
CREATE PERFETTO TABLE _critical_path_node AS
SELECT n.*, coalesce(n.ts - n.idle_dur, m.lane_min_ts, n.ts) AS idle_ts
FROM _critical_path_node_base AS n
LEFT JOIN _critical_path_null_lane_min AS m USING (lane)
ORDER BY
  n.id;

-- Terminal intervals on each lane (running spans and unattributed IRQ/kernel wakeups).
CREATE PERFETTO TABLE _critical_path_terminal AS
SELECT lane, ts, dur, id AS blocker_id FROM _critical_path_node WHERE dur > 0
UNION ALL
SELECT lane, idle_ts AS ts, ts - idle_ts AS dur, id AS blocker_id
FROM _critical_path_node
WHERE
  waker_id IS NULL
  AND prev_id IS NOT NULL
  AND idle_ts < ts;

-- Wakeup hop intervals delegating from a waiter lane to a waker target_lane.
CREATE PERFETTO TABLE _critical_path_hop AS
SELECT
  n.id,
  n.lane,
  n.idle_ts AS ts,
  min(n.ts, w.ts + w.dur) - n.idle_ts AS dur,
  min(n.ts, w.ts + w.dur) AS hop_end,
  w.lane AS target_lane,
  n.idle_dur IS NOT NULL AS is_root_hop
FROM _critical_path_node AS n
JOIN _critical_path_node AS w
  ON w.id = n.waker_id
WHERE
  min(n.ts, w.ts + w.dur) > n.idle_ts;

-- Flattened intervals per lane where at least one hop waits.
CREATE PERFETTO TABLE _critical_path_waited AS
FROM (SELECT target_lane AS lane, ts, dur FROM _critical_path_hop)
|> INTERVAL FLATTEN PER lane AGGREGATE COUNT(*) AS n;

-- Hops some other hop waits on, intersected with waiter coverage.
CREATE PERFETTO TABLE _critical_path_hop1_raw AS
INTERVAL INTERSECTION OF (_critical_path_hop AS g, _critical_path_waited AS w) PER lane
|> SELECT g.id, g.lane, ts, g.hop_end, g.target_lane;

-- Internal hops clamped to earliest waiter overlap.
CREATE PERFETTO TABLE _critical_path_hop1 AS
SELECT id, lane, min(ts) AS ts, hop_end - min(ts) AS dur, hop_end, target_lane
FROM _critical_path_hop1_raw
GROUP BY
  id;

-- 2-hop chains (c -> p) inside _critical_path_hop1.
CREATE PERFETTO TABLE _critical_path_edge AS
INTERVAL INTERSECTION OF (
  (
    SELECT target_lane AS lane, ts, dur, id AS hop_id, lane AS hop_lane
    FROM _critical_path_hop1
  ) AS c,
  (
    SELECT
      lane,
      ts,
      dur,
      id AS parent_hop_id,
      target_lane AS res_lane,
      hop_end AS p_end
    FROM _critical_path_hop1
  ) AS p
) PER lane
|> SELECT ts, dur, c.hop_id, c.hop_lane, p.parent_hop_id, p.res_lane, p.p_end;

-- Core hops that both wait on a hop in _critical_path_hop1 and are waited on.
CREATE PERFETTO TABLE _critical_path_core_id AS
SELECT hop_id AS id FROM _critical_path_edge
INTERSECT
SELECT parent_hop_id AS id FROM _critical_path_edge;

-- Waiter edges whose parent_hop_id is a core hop, clamped to the parent's outgoing edge spans.
CREATE PERFETTO TABLE _critical_path_waiter_clamped AS
INTERVAL INTERSECTION OF (
  (
    SELECT parent_hop_id AS mid_id, ts, dur, hop_id, hop_lane, ts AS hop_ts
    FROM _critical_path_edge
  ) AS e1,
  (
    SELECT
      hop_id AS mid_id,
      iif(
        parent_hop_id IN (SELECT id FROM _critical_path_core_id),
        ts,
        p_end - 1
      ) AS ts,
      iif(parent_hop_id IN (SELECT id FROM _critical_path_core_id), dur, 1) AS dur
    FROM _critical_path_edge
  ) AS e2
) PER mid_id
|> SELECT ts, dur, e1.hop_id, e1.hop_lane, e1.hop_ts, e1.mid_id AS parent_hop_id;

-- 1ns right-endpoint probes at each distinct p_end of outgoing edges from core hops.
CREATE PERFETTO TABLE _critical_path_probe AS
SELECT
  p_end - 1 AS ts,
  1 AS dur,
  p_end AS probe_end,
  row_number() OVER (ORDER BY p_end) - 1 AS probe_idx
FROM (
  SELECT DISTINCT p_end
  FROM _critical_path_edge
  WHERE
    hop_id IN (SELECT id FROM _critical_path_core_id)
);

-- Child tree nodes at each 1ns probe point.
CREATE PERFETTO TABLE _critical_path_child_slice AS
INTERVAL INTERSECTION OF (
  _critical_path_waiter_clamped AS e,
  _critical_path_probe AS p
)
|> SELECT
  e.hop_id,
  e.hop_lane,
  e.hop_ts,
  e.parent_hop_id,
  p.probe_end,
  p.probe_idx;

-- Root tree nodes at each 1ns probe point.
CREATE PERFETTO TABLE _critical_path_root_slice AS
SELECT
  e.hop_id,
  e.hop_lane,
  e.ts AS hop_ts,
  e.res_lane AS leaf_lane,
  p.probe_end,
  p.probe_idx
FROM _critical_path_edge AS e
JOIN _critical_path_probe AS p
  ON p.probe_end = e.p_end
WHERE
  e.hop_id IN (SELECT id FROM _critical_path_core_id);

-- Combined probe tree nodes for >= 3-hop internal chains.
CREATE PERFETTO TABLE _critical_path_tree_node AS
SELECT
  (hop_id << 20) | probe_idx AS id,
  (parent_hop_id << 20) | probe_idx AS raw_parent_id,
  hop_lane,
  hop_ts,
  probe_end,
  NULL AS leaf_lane
FROM _critical_path_child_slice
UNION ALL
SELECT
  (hop_id << 20) | probe_idx AS id,
  NULL AS raw_parent_id,
  hop_lane,
  hop_ts,
  probe_end,
  leaf_lane
FROM _critical_path_root_slice;

-- Propagate leaf_lane, chain_start, and edge_cnt down the probe tree.
CREATE PERFETTO TABLE _critical_path_chain3 AS
FROM (
  SELECT
    id,
    iif(
      raw_parent_id IN (SELECT id FROM _critical_path_tree_node),
      raw_parent_id,
      NULL
    ) AS parent_id,
    hop_lane,
    hop_ts,
    probe_end,
    leaf_lane
  FROM _critical_path_tree_node
)
|> TREE ACCUMULATE DOWN
  MIN(leaf_lane) AS res_lane,
  MAX(hop_ts) AS chain_start,
  COUNT(*) AS edge_cnt
|> SELECT hop_lane, res_lane, chain_start, probe_end, edge_cnt;

-- All internal hop chains (k = 1, k = 2, and k >= 3).
CREATE PERFETTO TABLE _critical_path_all_chains AS
SELECT target_lane AS lane, ts, dur, lane AS hop_lane, 1 AS depth
FROM _critical_path_hop1
UNION ALL
SELECT res_lane AS lane, ts, dur, hop_lane, 2 AS depth FROM _critical_path_edge
UNION ALL
SELECT
  res_lane AS lane,
  chain_start AS ts,
  probe_end - chain_start AS dur,
  hop_lane,
  edge_cnt + 1 AS depth
FROM _critical_path_chain3
WHERE
  res_lane IS NOT NULL
  AND edge_cnt >= 2;

-- Intersect internal hop chains with terminal intervals on res_lane.
CREATE PERFETTO TABLE _critical_path_internal_piece AS
INTERVAL INTERSECTION OF (
  _critical_path_all_chains AS c,
  _critical_path_terminal AS t
) PER lane
|> SELECT c.hop_lane AS lane, ts, dur, t.blocker_id, c.depth;

-- Resolved terminal blockers on every lane across all internal depths.
CREATE PERFETTO TABLE _critical_path_lane_res AS
SELECT lane, ts, dur, blocker_id, 0 AS depth FROM _critical_path_terminal
UNION ALL
SELECT lane, ts, dur, blocker_id, depth FROM _critical_path_internal_piece;

-- Resolved critical path pieces for all root hops (depth >= 1).
CREATE PERFETTO TABLE _critical_path_root_hop_res AS
INTERVAL INTERSECTION OF (
  (
    SELECT target_lane AS lane, ts, dur, id AS hop_id
    FROM _critical_path_hop
    WHERE
      is_root_hop
  ) AS h,
  _critical_path_lane_res AS r
) PER lane
|> SELECT ts, dur, h.hop_id AS root_id, r.depth, r.blocker_id;

CREATE PERFETTO INDEX _critical_path_root_hop_res_idx ON _critical_path_root_hop_res(
  root_id
);

-- Critical path for the given roots with the chain depth retained.
-- One row per `(root_id, depth, ts, dur, id, parent_id)` blocker
-- frame: `id` is the on-CPU blocker at this depth, `parent_id` is
-- the blocker one level up (the root at depth 0). At a self-wake the
-- woken thread is the depth-N fallback (it is in kernel) and the
-- waker chain layers at depth N+1. `_critical_path_by_roots`
-- collapses these depths to one blocker per `(root_id, ts)`.
CREATE PERFETTO MACRO _critical_path_with_depth_by_roots(
  _roots_table TableOrSubQuery,
  _node_table TableOrSubQuery
)
RETURNS TableOrSubQuery
AS (
  WITH
    _roots AS (
      SELECT DISTINCT
        root_node_id AS root_id
      FROM $_roots_table
    )
  SELECT
    n.id AS root_id,
    0 AS depth,
    n.ts,
    n.dur,
    n.id,
    n.id AS parent_id
  FROM _roots AS r
  JOIN _critical_path_node AS n
    ON n.id = r.root_id
  WHERE
    n.dur > 0
  UNION ALL
  SELECT
    n.id AS root_id,
    0 AS depth,
    n.idle_ts AS ts,
    n.idle_dur AS dur,
    n.id,
    n.id AS parent_id
  FROM _roots AS r
  JOIN _critical_path_node AS n
    ON n.id = r.root_id
  WHERE
    n.idle_dur > 0 AND n.waker_id IS NULL AND n.prev_id IS NOT NULL
  UNION ALL
  SELECT
    h.root_id,
    h.depth + 1 AS depth,
    h.ts,
    h.dur,
    h.blocker_id AS id,
    h.root_id AS parent_id
  FROM _roots AS r
  JOIN _critical_path_root_hop_res AS h
    ON h.root_id = r.root_id
);

-- Critical path for the given roots, one blocker per `(root_id, ts)`.
-- Pair with `_intervals_to_roots` to compute the path over a sparse
-- time region (e.g. binder transactions) without walking the whole
-- trace.
CREATE PERFETTO MACRO _critical_path_by_roots(
  _roots_table TableOrSubQuery,
  _node_table TableOrSubQuery
)
RETURNS TableOrSubQuery
AS (
  WITH
    _frames AS (
      SELECT
        *
      FROM _critical_path_with_depth_by_roots!($_roots_table, $_node_table)
    ),
    _root_spans AS (
      SELECT
        root_id,
        min(ts) AS root_ts,
        max(ts + dur) - min(ts) AS root_dur
      FROM _frames
      GROUP BY
        root_id
    ),
    _flatten_input AS (
      SELECT
        _frames.root_id,
        root_ts,
        root_dur,
        parent_id,
        id,
        ts,
        dur
      FROM _frames
      JOIN _root_spans
        USING (root_id)
    )
  SELECT
    flat.root_id,
    flat.id,
    flat.ts,
    flat.dur
  FROM _intervals_flatten!(_flatten_input) AS flat
  WHERE
    flat.dur > 0
  GROUP BY
    flat.root_id,
    flat.ts
);

-- Generates the critical path for only the time intervals for the utids given.
-- Currently expensive because of naive interval_intersect implementation.
-- Prefer _critical_paths_by_roots for performance. This is useful for a small
-- set of intervals, e.g app startups in a trace.
CREATE PERFETTO MACRO _critical_path_by_intervals(
  _intervals_table TableOrSubQuery,
  _node_table TableOrSubQuery
)
RETURNS TableOrSubQuery
AS (
  WITH
    _nodes AS (
      SELECT
        *
      FROM $_node_table
    ),
    _intervals AS (
      SELECT
        row_number() OVER (ORDER BY ts) AS id,
        ts,
        dur,
        utid AS root_utid
      FROM $_intervals_table
    ),
    _critical_path AS (
      SELECT
        row_number() OVER (ORDER BY ts) AS id,
        root_id,
        id AS cr_id,
        ts,
        dur
      FROM _critical_path_by_roots!(
      _intervals_to_roots!($_intervals_table, $_node_table),
      _nodes)
    ),
    _span AS (
      SELECT
        _root_nodes.utid AS root_utid,
        _nodes.utid,
        cr.root_id,
        cr.cr_id,
        cr.id,
        cr.ts,
        cr.dur
      FROM _critical_path AS cr
      JOIN _nodes AS _root_nodes
        ON _root_nodes.id = cr.root_id
      JOIN _nodes
        ON _nodes.id = cr.cr_id
    )
  SELECT DISTINCT
    _span.root_utid,
    _span.utid,
    _span.root_id,
    _span.cr_id AS id,
    ii.ts,
    ii.dur,
    _intervals.ts AS interval_ts,
    _intervals.dur AS interval_dur
  FROM _interval_intersect!((_span, _intervals), (root_utid)) AS ii
  JOIN _span
    ON _span.id = ii.id_0
  JOIN _intervals
    ON _intervals.id = ii.id_1
);

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
SELECT root_utid, root_id, id, ts, dur, utid
FROM _critical_path_by_intervals!(
  (SELECT $root_utid AS utid, $ts AS ts, $dur AS dur),
  _wakeup_graph
);
