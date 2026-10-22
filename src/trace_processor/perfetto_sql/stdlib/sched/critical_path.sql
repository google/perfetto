--
-- Copyright 2026 The Android Open Source Project
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

-- Critical path building blocks shared by `sched.thread_executing_span` and
-- domain specific "work passing" critical paths.
--
-- `_critical_path_with_depth_by_roots!` is the thin SQL wrapper around the C++
-- walker (`__intrinsic_wakeup_graph_agg` + `__intrinsic_critical_path_walk`).
-- It is deliberately free of any trace-wide tables so that including this
-- module is cheap.
--
-- Domain modules describe causality as a table of work-passing edges:
--
--   (ready_ts, ready_utid, ts, utid, type, info)
--
-- meaning "`ready_utid` made `utid` ready at `ready_ts`, and `utid` started
-- the work at `ts`". Examples: a scheduler wakeup, a binder transaction or
-- reply, a vsync callback. The rules themselves live in domain modules
-- (e.g. `android.critical_path`); this module only turns them into a wakeup
-- graph and walks it with the same C++ walker used by
-- `sched.thread_executing_span`.

INCLUDE PERFETTO MODULE intervals.intersect;

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
  SELECT
    c0 AS root_id,
    c1 AS depth,
    c2 AS ts,
    c3 AS dur,
    c4 AS id,
    c6 AS parent_id
  FROM __intrinsic_table_ptr(
    __intrinsic_critical_path_walk(
      (
        SELECT
          __intrinsic_wakeup_graph_agg(id, utid, ts, dur, idle_dur, waker_id, prev_id)
        FROM $_node_table
      ),
      (
        SELECT
          __intrinsic_array_agg(root_node_id)
        FROM $_roots_table
      )
    )
  )
  WHERE
    __intrinsic_table_ptr_bind(c0, 'root_id')
    AND __intrinsic_table_ptr_bind(c1, 'depth')
    AND __intrinsic_table_ptr_bind(c2, 'ts')
    AND __intrinsic_table_ptr_bind(c3, 'dur')
    AND __intrinsic_table_ptr_bind(c4, 'blocker_id')
    AND __intrinsic_table_ptr_bind(c5, 'blocker_utid')
    AND __intrinsic_table_ptr_bind(c6, 'parent_id')
);

-- Converts a work-passing edge table into a wakeup graph node table.
--
-- Every edge becomes one node `id` on `utid`. Its `waker_id` is the node that
-- was active on `ready_utid` at `ready_ts`, i.e. the latest node on
-- `ready_utid` with `ts <= ready_ts`. All `waker_id`s are resolved in a
-- single window-function pass instead of one lookup per hop.
--
-- Node ids are ordered by `ts`, so for well formed edges (`ready_ts <= ts`)
-- `waker_id < id` always holds. Edges violating it are dropped: this keeps
-- the graph acyclic, which the walker relies on to terminate.
CREATE PERFETTO MACRO _critical_path_pass_work_graph(
  -- Table with columns (ready_ts, ready_utid, ts, utid, type, info).
  pass_work TableOrSubQuery
)
-- (id, utid, ready_ts, ts, ready_utid, ready_type, ready_info, waker_id).
RETURNS TableOrSubQuery
AS (
  WITH
    _nodes AS (
      SELECT
        row_number() OVER (ORDER BY ts, ready_ts, utid) - 1 AS id,
        utid,
        ready_ts,
        ts,
        ready_utid,
        type AS ready_type,
        info AS ready_info
      FROM $pass_work
    ),
    -- Interleave "node starts at ts on utid" events with one "who was active
    -- on ready_utid at ready_ts?" lookup per node. Sorted per thread, the
    -- answer to a lookup is the last node event before it. Lookups carry the
    -- node's own columns so no join is needed afterwards.
    _events AS (
      SELECT
        utid AS part,
        ts AS event_ts,
        id AS sort_key,
        0 AS is_lookup,
        id AS node_id,
        NULL AS id,
        NULL AS utid,
        NULL AS ready_ts,
        NULL AS ts,
        NULL AS ready_utid,
        NULL AS ready_type,
        NULL AS ready_info
      FROM _nodes
      UNION ALL
      SELECT
        ready_utid AS part,
        ready_ts AS event_ts,
        -- Sorts after earlier nodes with the same timestamp but before the
        -- node itself, so a node never resolves to itself.
        id - 0.5 AS sort_key,
        1 AS is_lookup,
        NULL AS node_id,
        id,
        utid,
        ready_ts,
        ts,
        ready_utid,
        ready_type,
        ready_info
      FROM _nodes
    ),
    _resolved AS (
      SELECT
        *,
        max(node_id) OVER (
          PARTITION BY part
          ORDER BY event_ts, sort_key
          ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
        ) AS last_node_id
      FROM _events
    )
  SELECT
    id,
    utid,
    ready_ts,
    ts,
    ready_utid,
    ready_type,
    ready_info,
    iif(ready_utid IS NOT NULL AND last_node_id < id, last_node_id, NULL)
      AS waker_id
  FROM _resolved
  WHERE is_lookup
  ORDER BY id
);

-- Walks a `_critical_path_pass_work_graph!` backwards from the end of each
-- interval in `intervals`.
--
-- Mapping onto the C++ walker (see `critical_path.cc`):
--  * A node starts at its `ready_ts`: from the moment work is passed to a
--    thread, that thread is responsible for the time until it passes work on
--    (this includes runnable / wait latency, as in `thread_executing_span`).
--  * `idle_dur` is NULL: a node's idle window is open, so all time before it
--    is delegated to its `waker_id`. `prev_id` is NULL: there is no
--    same-thread fallback, a chain simply ends when there is no waker.
--  * Each interval gets a synthetic root node at `ts + dur` whose idle window
--    is exactly `[ts, ts + dur)` and whose waker is the node active on `utid`
--    at the end of the interval. This lets many intervals be walked in one
--    call without mutating shared nodes.
--
-- Every emitted frame is one hop: the node that was responsible for
-- `[ts, ts + dur)` of the interval, at `depth` hops from the end.
CREATE PERFETTO MACRO _critical_path_pass_work_by_intervals(
  -- Table with columns (id, ts, dur, utid). `id` must be unique.
  intervals TableOrSubQuery,
  -- Output of `_critical_path_pass_work_graph!`.
  graph TableOrSubQuery
)
-- (interval_id, interval_ts, depth, id, ts, dur) where `id` is the graph node
-- id and `depth` is the number of hops from the end of the interval.
RETURNS TableOrSubQuery
AS (
  WITH
    _graph AS (
      SELECT * FROM $graph
    ),
    _intervals AS (
      SELECT
        row_number() OVER (ORDER BY id) - 1 AS idx,
        id AS interval_id,
        ts,
        dur,
        utid
      FROM $intervals
      WHERE dur > 0
    ),
    _bounds AS (
      SELECT
        (SELECT count(*) FROM _graph) AS root_base,
        (SELECT max(ts + dur) FROM _intervals) AS max_end
    ),
    -- Node active on `utid` at the end of each interval (same rule as
    -- `waker_id` resolution).
    _end_nodes AS (
      SELECT
        i.idx,
        (
          SELECT max(g.id)
          FROM _graph AS g
          WHERE g.utid = i.utid AND g.ts <= i.ts + i.dur
        ) AS end_node_id
      FROM _intervals AS i
    ),
    _walk_nodes AS (
      SELECT
        g.id,
        g.utid,
        g.ready_ts AS ts,
        b.max_end - g.ready_ts AS dur,
        NULL AS idle_dur,
        g.waker_id,
        NULL AS prev_id
      FROM _graph AS g, _bounds AS b
      -- Nodes starting after the last interval end can never be visited.
      WHERE g.ts <= b.max_end
      UNION ALL
      SELECT
        b.root_base + i.idx AS id,
        i.utid,
        i.ts + i.dur AS ts,
        0 AS dur,
        i.dur AS idle_dur,
        e.end_node_id AS waker_id,
        NULL AS prev_id
      FROM _intervals AS i
      JOIN _end_nodes AS e USING (idx), _bounds AS b
    ),
    _roots AS (
      SELECT b.root_base + i.idx AS root_node_id
      FROM _intervals AS i, _bounds AS b
    )
  SELECT
    i.interval_id,
    i.ts AS interval_ts,
    w.depth - 1 AS depth,
    w.id,
    w.ts,
    w.dur
  FROM _critical_path_with_depth_by_roots!(_roots, _walk_nodes) AS w
  JOIN _bounds AS b
  JOIN _intervals AS i
    ON i.idx = w.root_id - b.root_base
  WHERE w.id != w.root_id
);

-- Per-hop critical path over `intervals`, enriched with the readiness
-- metadata of each hop and a breakdown of the hop's thread states.
--
-- For each hop, `[ready_ts, ts)` is the time between the thread being made
-- ready and starting the work, and `[ts, ts + dur)` is the time it spent
-- until it passed work to the next hop (or the interval ended).
CREATE PERFETTO MACRO _critical_path_pass_work_hops(
  -- Table with columns (id, ts, dur, utid). `id` must be unique.
  intervals TableOrSubQuery,
  -- Output of `_critical_path_pass_work_graph!`.
  graph TableOrSubQuery
)
-- (interval_id, depth, ready_ts, ts, dur, utid, ready_utid, ready_type,
--  ready_info, running_dur, runnable_dur, sleeping_dur,
--  uninterruptible_sleep_dur).
RETURNS TableOrSubQuery
AS (
  WITH
    _frames AS (
      SELECT * FROM _critical_path_pass_work_by_intervals!($intervals, $graph)
    ),
    _hops AS (
      SELECT
        row_number() OVER (ORDER BY f.interval_id, f.depth DESC) AS id,
        f.interval_id,
        f.depth,
        iif(g.ready_ts > f.interval_ts, g.ready_ts, NULL) AS ready_ts,
        max(f.ts, g.ts) AS ts,
        f.ts + f.dur - max(f.ts, g.ts) AS dur,
        g.utid,
        g.ready_utid,
        g.ready_type,
        g.ready_info
      FROM _frames AS f
      JOIN $graph AS g USING (id)
    ),
    _hop_spans AS (
      SELECT id, ts, dur, utid FROM _hops WHERE dur > 0
    ),
    -- Only the thread states that can overlap a hop.
    _states AS (
      SELECT s.id, s.ts, s.dur, s.utid, s.state
      FROM thread_state AS s
      WHERE s.dur > 0
        AND s.utid IN (SELECT utid FROM _hop_spans)
        AND s.ts < (SELECT max(ts + dur) FROM _hop_spans)
        AND s.ts + s.dur > (SELECT min(ts) FROM _hop_spans)
    ),
    _state_durs AS (
      SELECT
        ii.id_0 AS id,
        sum(iif(s.state = 'Running', ii.dur, 0)) AS running_dur,
        sum(iif(s.state IN ('R', 'R+'), ii.dur, 0)) AS runnable_dur,
        sum(iif(s.state = 'S', ii.dur, 0)) AS sleeping_dur,
        sum(iif(s.state = 'D', ii.dur, 0)) AS uninterruptible_sleep_dur
      FROM _interval_intersect!((_hop_spans, _states), (utid)) AS ii
      JOIN _states AS s
        ON s.id = ii.id_1
      GROUP BY ii.id_0
    )
  SELECT
    h.interval_id,
    h.depth,
    h.ready_ts,
    h.ts,
    h.dur,
    h.utid,
    h.ready_utid,
    h.ready_type,
    h.ready_info,
    coalesce(d.running_dur, 0) AS running_dur,
    coalesce(d.runnable_dur, 0) AS runnable_dur,
    coalesce(d.sleeping_dur, 0) AS sleeping_dur,
    coalesce(d.uninterruptible_sleep_dur, 0) AS uninterruptible_sleep_dur
  FROM _hops AS h
  LEFT JOIN _state_durs AS d USING (id)
  ORDER BY h.interval_id, h.depth DESC
);
