// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {STITCHED_CORE_MODULE} from './core_sql';
import type {StitchedSqlModule} from './sql_modules';

// Slices stitched through the scheduler's critical path: while a thread
// sleeps, the threads it waits on (transitively, as computed by
// sched.thread_executing_span) run in its place.
//
// The critical path of each sleep is split in "pieces": one blocker thread
// running for part of the sleep. The slices blockers ran during the pieces
// hanging below the same node are merged by (thread, name path) below it,
// as the flamegraph would. Node ids:
//   slice id          slices of the selected / sleeping threads
//   2^32 + n          blocker slices below a node
//   2^33 + n          blocker time outside any slice below a node
//   2^60 + utid       a thread sleeping outside any slice
//
// Downstream steps, each materialized by the caller:
//   threads  = _stitched_cp_threads!(tracks)
//   seeds    = _stitched_cp_seeds!(tracks, start, end)
//   roots    = _stitched_cp_down_roots!(threads, start, end)
//   all      = _critical_path_with_depth_by_roots!(roots, _wakeup_graph)
//   frames   = _stitched_cp_window_frames!(all, roots, start, end)
//   flat     = _stitched_cp_flatten!(frames)
//   pieces   = _stitched_cp_pieces!(flat, threads, FALSE, start, end)
//   blocked  = _stitched_cp_blocked_slice!(roots)
//   attached = _stitched_cp_attach!(pieces, blocked)
//   hits     = _stitched_cp_hits!(attached)
//   canon    = _stitched_cp_canonical!(hits)
//   placed   = _stitched_cp_place!(hits, attached, canon)
//   groups   = _stitched_cp_blocker_groups!(placed, canon)
//   covered  = _stitched_cp_piece_covered!(placed)
//   nodes    = _stitched_cp_down_nodes!(seeds, attached, groups, covered)
//
// Upstream steps:
//   threads  = _stitched_cp_threads!(tracks)
//   roots    = _stitched_cp_up_roots!(threads, start, end)
//   all      = _critical_path_with_depth_by_roots!(roots, _wakeup_graph)
//   hit      = _stitched_cp_hit_roots!(all, threads, start, end)
//   frames   = _stitched_cp_window_frames!(all, hit, start, end)
//   flat     = _stitched_cp_flatten!(frames)
//   pieces   = _stitched_cp_pieces!(flat, threads, TRUE, start, end)
//   blocked  = _stitched_cp_canonical_blocked!(
//                _stitched_cp_blocked_slice!(hit))
//   attached = _stitched_cp_attach!(pieces, blocked)
//   hits     = _stitched_cp_hits!(attached)
//   canon    = _stitched_cp_canonical!(hits)
//   placed   = _stitched_cp_place!(hits, attached, canon)
//   groups   = _stitched_cp_blocker_groups!(placed, canon)
//   covered  = _stitched_cp_piece_covered!(placed)
//   nodes    = _stitched_cp_up_nodes!(blocked, attached, groups, covered)
export const STITCHED_CRITICAL_PATH_MODULE: StitchedSqlModule = {
  name: 'critical_path',
  deps: [STITCHED_CORE_MODULE],
  sql: `
INCLUDE PERFETTO MODULE sched.thread_executing_span;
INCLUDE PERFETTO MODULE intervals.intersect;
INCLUDE PERFETTO MODULE intervals.overlap;

CREATE PERFETTO INDEX _stitched_wakeup_graph_id_idx ON _wakeup_graph(id);

-- The threads ("utid") of the $tracks (a table with a "track_id" column).
CREATE PERFETTO MACRO _stitched_cp_threads(tracks TableOrSubquery)
RETURNS TableOrSubquery AS (
  SELECT DISTINCT t.utid
  FROM $tracks x
  JOIN _stitched_track t USING (track_id)
  WHERE t.utid IS NOT NULL
);

-- The slices of the thread $tracks overlapping [$start_ts, $end_ts), with
-- their time clipped to it.
CREATE PERFETTO MACRO _stitched_cp_seeds(
  tracks TableOrSubquery,
  start_ts Expr,
  end_ts Expr
)
RETURNS TableOrSubquery AS (
  SELECT
    s.id,
    s.name,
    s.parent_id,
    t.utid,
    min(iif(s.dur < 0, $end_ts, s.ts + s.dur), $end_ts)
      - max(s.ts, $start_ts) AS cdur
  FROM $tracks x
  JOIN _stitched_track t USING (track_id)
  JOIN slice s USING (track_id)
  WHERE t.utid IS NOT NULL
    AND s.ts < $end_ts
    AND iif(s.dur < 0, $end_ts, s.ts + s.dur) > $start_ts
);

-- Wakeups ending a sleep of the $threads overlapping [$start_ts, $end_ts).
CREATE PERFETTO MACRO _stitched_cp_down_roots(
  threads TableOrSubquery,
  start_ts Expr,
  end_ts Expr
)
RETURNS TableOrSubquery AS (
  SELECT w.id AS root_node_id
  FROM _wakeup_graph w
  JOIN $threads USING (utid)
  WHERE w.idle_dur > 0 AND w.ts > $start_ts AND w.ts - w.idle_dur < $end_ts
);

-- Wakeups ending a sleep of any thread but the $threads overlapping
-- [$start_ts, $end_ts): the candidates for being blocked on the $threads.
CREATE PERFETTO MACRO _stitched_cp_up_roots(
  threads TableOrSubquery,
  start_ts Expr,
  end_ts Expr
)
RETURNS TableOrSubquery AS (
  SELECT w.id AS root_node_id
  FROM _wakeup_graph w
  LEFT JOIN $threads t USING (utid)
  WHERE t.utid IS NULL
    AND w.idle_dur > 0
    AND w.ts > $start_ts
    AND w.ts - w.idle_dur < $end_ts
);

-- The roots ("root_node_id") of the critical path $frames with a $threads
-- blocker during [$start_ts, $end_ts).
CREATE PERFETTO MACRO _stitched_cp_hit_roots(
  frames TableOrSubquery,
  threads TableOrSubquery,
  start_ts Expr,
  end_ts Expr
)
RETURNS TableOrSubquery AS (
  SELECT DISTINCT f.root_id AS root_node_id
  FROM $frames f
  JOIN _wakeup_graph b ON b.id = f.id
  JOIN $threads USING (utid)
  WHERE f.ts < $end_ts AND f.ts + f.dur > $start_ts
);

-- The critical path $frames of the $roots (a table indexed by
-- "root_node_id") overlapping [$start_ts, $end_ts): only those can make
-- pieces, so the rest is dropped before flattening.
CREATE PERFETTO MACRO _stitched_cp_window_frames(
  frames TableOrSubquery,
  roots TableOrSubquery,
  start_ts Expr,
  end_ts Expr
)
RETURNS TableOrSubquery AS (
  SELECT f.*
  FROM $frames f
  JOIN $roots r ON r.root_node_id = f.root_id
  WHERE f.ts < $end_ts AND f.ts + f.dur > $start_ts
);

-- One on-CPU blocker per (root, time) out of the critical path $frames
-- (as returned by _critical_path_with_depth_by_roots), as done by stdlib's
-- _critical_path_by_roots. Only the roots whose frames overlap need
-- flattening (rare: self-wakeups): the frames of the others are kept as is,
-- which avoids the cost of flattening every frame.
CREATE PERFETTO MACRO _stitched_cp_flatten(frames TableOrSubquery)
RETURNS TableOrSubquery AS (
  WITH
    overlapping AS MATERIALIZED (
      SELECT root_id
      FROM (
        SELECT
          root_id,
          ts,
          max(ts + dur) OVER (
            PARTITION BY root_id
            ORDER BY ts, depth
            ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
          ) AS prev_end
        FROM $frames
      )
      WHERE ts < prev_end
      GROUP BY root_id
    ),
    nested AS MATERIALIZED (
      SELECT f.*
      FROM $frames f
      JOIN overlapping USING (root_id)
    ),
    root_spans AS MATERIALIZED (
      SELECT root_id, min(ts) AS root_ts, max(ts + dur) - min(ts) AS root_dur
      FROM nested
      GROUP BY root_id
    ),
    flatten_input AS MATERIALIZED (
      SELECT f.root_id, root_ts, root_dur, parent_id, id, ts, dur
      FROM nested f
      JOIN root_spans USING (root_id)
    )
  SELECT f.root_id, f.id, f.ts, f.dur
  FROM $frames f
  LEFT JOIN overlapping o USING (root_id)
  WHERE o.root_id IS NULL AND f.dur > 0
  UNION ALL
  SELECT flat.root_id, flat.id, flat.ts, flat.dur
  FROM _intervals_flatten!(flatten_input) flat
  WHERE flat.dur > 0
  GROUP BY flat.root_id, flat.ts
);

-- Pieces of critical path out of the $flat critical paths: for each sleep,
-- each blocker thread running in its place, clipped to the sleep and to
-- [$start_ts, $end_ts). Blockers which are the sleeping thread itself (e.g.
-- self-wakeups) are not pieces: that time is the thread's own. If
-- $only_threads, only pieces with a $threads blocker are kept.
CREATE PERFETTO MACRO _stitched_cp_pieces(
  flat TableOrSubquery,
  threads TableOrSubquery,
  only_threads Expr,
  start_ts Expr,
  end_ts Expr
)
RETURNS TableOrSubquery AS (
  WITH p AS (
    SELECT
      f.root_id,
      r.utid AS root_utid,
      b.utid AS blocker_utid,
      max(f.ts, r.ts - r.idle_dur, $start_ts) AS ts,
      min(f.ts + f.dur, r.ts, $end_ts) AS end_ts
    FROM $flat f
    JOIN _wakeup_graph r ON r.id = f.root_id
    JOIN _wakeup_graph b ON b.id = f.id
    LEFT JOIN $threads t ON t.utid = b.utid
    WHERE b.utid != r.utid AND (NOT $only_threads OR t.utid IS NOT NULL)
  )
  SELECT
    row_number() OVER (ORDER BY root_id, ts) AS piece_id,
    root_id,
    root_utid,
    blocker_utid,
    ts,
    end_ts - ts AS dur
  FROM p
  WHERE end_ts > ts
);

-- For each wakeup in $roots (a table with "root_node_id"), the deepest slice
-- its thread was in while sleeping ("slice_id", NULL if none).
CREATE PERFETTO MACRO _stitched_cp_blocked_slice(roots TableOrSubquery)
RETURNS TableOrSubquery AS (
  WITH
    w AS MATERIALIZED (
      SELECT x.root_node_id AS id, g.ts - 1 AS ts, 1 AS dur, g.utid
      FROM $roots x
      JOIN _wakeup_graph g ON g.id = x.root_node_id
    ),
    bounds AS MATERIALIZED (
      SELECT utid, min(ts) AS min_ts, max(ts) + 1 AS max_ts FROM w GROUP BY utid
    ),
    candidates AS MATERIALIZED (
      SELECT s.id, s.ts, iif(s.dur < 0, b.max_ts - s.ts, s.dur) AS dur, b.utid
      FROM bounds b
      JOIN thread_track t USING (utid)
      JOIN slice s ON s.track_id = t.id
      WHERE s.dur != 0
        AND s.ts < b.max_ts
        AND iif(s.dur < 0, b.max_ts, s.ts + s.dur) > b.min_ts
    ),
    hits AS MATERIALIZED (
      SELECT
        ii.id_0 AS root_node_id,
        ii.id_1 AS slice_id,
        row_number() OVER (PARTITION BY ii.id_0 ORDER BY s.depth DESC) AS rn
      FROM _interval_intersect!((w, candidates), (utid)) ii
      JOIN slice s ON s.id = ii.id_1
    )
  SELECT x.root_node_id, h.slice_id
  FROM $roots x
  LEFT JOIN hits h ON h.root_node_id = x.root_node_id AND h.rn = 1
);

-- The $pieces with the node they hang below ("parent_node_id"): the slice
-- their root was blocked in ($blocked, indexed by "root_node_id"), or a node
-- for the root's thread when it slept outside any slice.
CREATE PERFETTO MACRO _stitched_cp_attach(
  pieces TableOrSubquery,
  blocked TableOrSubquery
)
RETURNS TableOrSubquery AS (
  SELECT
    p.*,
    coalesce(b.slice_id, 1152921504606846976 + p.root_utid) AS parent_node_id
  FROM $pieces p
  JOIN $blocked b ON b.root_node_id = p.root_id
);

-- The blocker slices overlapping each of the $pieces, clipped to the piece.
CREATE PERFETTO MACRO _stitched_cp_hits(pieces TableOrSubquery)
RETURNS TableOrSubquery AS (
  WITH
    piece_ii AS MATERIALIZED (
      SELECT piece_id AS id, ts, dur, blocker_utid AS utid FROM $pieces
    ),
    bounds AS MATERIALIZED (
      SELECT utid, min(ts) AS min_ts, max(ts + dur) AS max_ts
      FROM piece_ii
      GROUP BY utid
    ),
    blocker_slices AS MATERIALIZED (
      SELECT s.id, s.ts, iif(s.dur < 0, b.max_ts - s.ts, s.dur) AS dur, b.utid
      FROM bounds b
      JOIN thread_track t USING (utid)
      JOIN slice s ON s.track_id = t.id
      WHERE s.dur != 0
        AND s.ts < b.max_ts
        AND iif(s.dur < 0, b.max_ts, s.ts + s.dur) > b.min_ts
    )
  SELECT ii.id_0 AS piece_id, ii.id_1 AS slice_id, ii.dur
  FROM _interval_intersect!((piece_ii, blocker_slices), (utid)) ii
);

-- Canonical ids of the $slices (a table with a "slice_id" column): one per
-- (thread, path of slice names from the root). The flamegraph merges such
-- slices below the same node anyway: merging them first keeps the many
-- copies of the slices of a busy blocker (e.g. a server blocking many
-- clients) from being built and carried through to the flamegraph.
CREATE PERFETTO MACRO _stitched_cp_canonical_ids(slices TableOrSubquery)
RETURNS TableOrSubquery AS (
  WITH
    xs AS MATERIALIZED (
      SELECT DISTINCT slice_id FROM $slices
    ),
    paths AS MATERIALIZED (
      SELECT xs.slice_id, group_concat(a.name, char(31) ORDER BY a.depth) AS path
      FROM xs
      JOIN ancestor_slice(xs.slice_id) a
      GROUP BY xs.slice_id
    ),
    keyed AS (
      SELECT
        xs.slice_id,
        s.parent_id,
        s.name,
        coalesce(l.label, '') || char(30) || coalesce(p.path, '') || char(31)
          || coalesce(s.name, '') AS path_key
      FROM xs
      JOIN slice s ON s.id = xs.slice_id
      LEFT JOIN paths p USING (slice_id)
      LEFT JOIN _stitched_track t USING (track_id)
      LEFT JOIN _stitched_thread_label l
        ON l.thread_key = coalesce(t.utid * 2, t.upid * 2 + 1)
    )
  SELECT
    slice_id,
    parent_id,
    name,
    min(slice_id) OVER (PARTITION BY path_key) AS canon_id
  FROM keyed
);

-- Canonical ids (see _stitched_cp_canonical_ids) of the slices of the $hits,
-- with the canonical id of their parent. A slice overlapping a piece implies
-- its parent does too, so parents are slices of the $hits as well.
CREATE PERFETTO MACRO _stitched_cp_canonical(hits TableOrSubquery)
RETURNS TableOrSubquery AS (
  WITH canon AS MATERIALIZED (
    SELECT * FROM _stitched_cp_canonical_ids!($hits)
  )
  SELECT c.slice_id, c.canon_id, pc.canon_id AS canon_parent_id, c.name
  FROM canon c
  LEFT JOIN canon pc ON pc.slice_id = c.parent_id
);

-- The $blocked slices (as returned by _stitched_cp_blocked_slice) replaced by
-- their canonical ids (see _stitched_cp_canonical_ids), so that the blockers
-- of all the slices blocked at the same place merge early.
CREATE PERFETTO MACRO _stitched_cp_canonical_blocked(blocked TableOrSubquery)
RETURNS TableOrSubquery AS (
  WITH canon AS MATERIALIZED (
    SELECT *
    FROM _stitched_cp_canonical_ids!(
      (SELECT slice_id FROM $blocked WHERE slice_id IS NOT NULL)
    )
  )
  SELECT b.root_node_id, c.canon_id AS slice_id
  FROM $blocked b
  LEFT JOIN canon c USING (slice_id)
);

-- The $hits placed in the tree: the node their piece hangs below (from the
-- $pieces of _stitched_cp_attach, indexed by "piece_id") and their
-- $canonical slice and parent (indexed by "slice_id").
CREATE PERFETTO MACRO _stitched_cp_place(
  hits TableOrSubquery,
  pieces TableOrSubquery,
  canonical TableOrSubquery
)
RETURNS TableOrSubquery AS (
  SELECT
    h.piece_id,
    p.parent_node_id,
    p.blocker_utid AS utid,
    h.slice_id,
    c.canon_id,
    c.canon_parent_id,
    h.dur
  FROM $hits h
  JOIN $pieces p USING (piece_id)
  JOIN $canonical c USING (slice_id)
);

-- The $placed hits merged across the pieces hanging below the same node:
-- one group per (parent_node_id, canonical slice), weighted by the time of
-- its slices during the pieces minus the time of their children.
CREATE PERFETTO MACRO _stitched_cp_blocker_groups(
  placed TableOrSubquery,
  canonical TableOrSubquery
)
RETURNS TableOrSubquery AS (
  WITH
    total AS (
      SELECT
        parent_node_id,
        canon_id,
        canon_parent_id,
        min(utid) AS utid,
        sum(dur) AS dur,
        count(DISTINCT slice_id) AS slices,
        group_concat(DISTINCT slice_id) AS slice_ids
      FROM $placed
      GROUP BY parent_node_id, canon_id
    ),
    children AS MATERIALIZED (
      SELECT parent_node_id, canon_parent_id AS canon_id, sum(dur) AS dur
      FROM $placed
      WHERE canon_parent_id IS NOT NULL
      GROUP BY parent_node_id, canon_parent_id
    )
  SELECT
    4294967296 + row_number() OVER () AS gid,
    t.parent_node_id,
    t.canon_id AS slice_id,
    t.canon_parent_id AS parent_id,
    c.name,
    t.utid,
    t.slice_ids,
    max(t.dur - coalesce(ch.dur, 0), 0) AS self_dur,
    t.slices AS self_count
  FROM total t
  JOIN $canonical c ON c.slice_id = t.canon_id
  LEFT JOIN children ch
    ON ch.parent_node_id = t.parent_node_id AND ch.canon_id = t.canon_id
);

-- The time of each piece covered by blocker slices, out of the $placed hits.
CREATE PERFETTO MACRO _stitched_cp_piece_covered(placed TableOrSubquery)
RETURNS TableOrSubquery AS (
  SELECT piece_id, sum(dur) AS dur
  FROM $placed
  WHERE canon_parent_id IS NULL
  GROUP BY piece_id
);

-- Stitched nodes of the blocker slice $groups (indexed by
-- (parent_node_id, slice_id)) below the nodes the $pieces hang below. A
-- slice overlapping a piece implies its parent does too, so parents are
-- always in the same group. The pieces' time outside any blocker slice
-- ($covered, indexed by "piece_id") is a "(running outside any slice)" node
-- per blocker thread.
CREATE PERFETTO MACRO _stitched_cp_blocker_nodes(
  pieces TableOrSubquery,
  groups TableOrSubquery,
  covered TableOrSubquery
)
RETURNS TableOrSubquery AS (
  WITH uncovered AS (
    SELECT
      p.parent_node_id,
      min(p.blocker_utid) AS utid,
      sum(p.dur - coalesce(c.dur, 0)) AS dur
    FROM $pieces p
    LEFT JOIN $covered c USING (piece_id)
    LEFT JOIN _stitched_thread_label l ON l.thread_key = p.blocker_utid * 2
    GROUP BY p.parent_node_id, l.label
  )
  SELECT
    g.gid AS id,
    coalesce(pg.gid, g.parent_node_id) AS parent_id,
    g.name,
    g.slice_id,
    g.slice_ids,
    g.utid,
    NULL AS upid,
    pg.gid IS NULL AS is_stitch,
    'slice' AS kind,
    g.self_dur,
    g.self_count
  FROM $groups g
  LEFT JOIN $groups pg
    ON pg.parent_node_id = g.parent_node_id AND pg.slice_id = g.parent_id
  UNION ALL
  SELECT
    8589934592 + row_number() OVER () AS id,
    parent_node_id AS parent_id,
    '(running outside any slice)' AS name,
    NULL AS slice_id,
    NULL AS slice_ids,
    utid,
    NULL AS upid,
    TRUE AS is_stitch,
    'slice' AS kind,
    dur AS self_dur,
    0 AS self_count
  FROM uncovered
  WHERE dur > 0
);

-- Nodes for the threads of the $pieces which slept outside any slice.
CREATE PERFETTO MACRO _stitched_cp_thread_nodes(pieces TableOrSubquery)
RETURNS TableOrSubquery AS (
  SELECT DISTINCT
    parent_node_id AS id,
    NULL AS parent_id,
    '(blocked outside any slice)' AS name,
    NULL AS slice_id,
    NULL AS slice_ids,
    root_utid AS utid,
    NULL AS upid,
    FALSE AS is_stitch,
    'slice' AS kind,
    0 AS self_dur,
    0 AS self_count
  FROM $pieces
  WHERE parent_node_id >= 1152921504606846976
);

-- Downstream nodes: the $seeds (indexed by "id") and, below the slice each
-- sleep happened in, the slices on its critical path. Weights are time on
-- the critical path: a seed's own time excludes its nested seeds and the
-- critical path pieces hanging below it.
CREATE PERFETTO MACRO _stitched_cp_down_nodes(
  seeds TableOrSubquery,
  pieces TableOrSubquery,
  groups TableOrSubquery,
  covered TableOrSubquery
)
RETURNS TableOrSubquery AS (
  WITH
    blocked_time AS (
      SELECT parent_node_id AS id, sum(dur) AS dur
      FROM $pieces
      GROUP BY parent_node_id
    ),
    seed_child AS (
      SELECT parent_id AS id, sum(cdur) AS dur
      FROM $seeds
      WHERE parent_id IS NOT NULL
      GROUP BY parent_id
    )
  SELECT
    s.id,
    s.parent_id,
    s.name,
    s.id AS slice_id,
    cast(s.id AS TEXT) AS slice_ids,
    s.utid,
    NULL AS upid,
    FALSE AS is_stitch,
    'slice' AS kind,
    max(s.cdur - coalesce(sc.dur, 0) - coalesce(bt.dur, 0), 0) AS self_dur,
    1 AS self_count
  FROM $seeds s
  LEFT JOIN seed_child sc ON sc.id = s.id
  LEFT JOIN blocked_time bt ON bt.id = s.id
  UNION ALL
  SELECT * FROM _stitched_cp_thread_nodes!($pieces)
  UNION ALL
  SELECT * FROM _stitched_cp_blocker_nodes!($pieces, $groups, $covered)
);

-- Upstream nodes: for each thread blocked on the selected threads, the
-- slices it was blocked in ($blocked) and, below them, the selected threads'
-- slices running on its critical path. Weights are time on the blocked
-- threads' critical paths.
CREATE PERFETTO MACRO _stitched_cp_up_nodes(
  blocked TableOrSubquery,
  pieces TableOrSubquery,
  groups TableOrSubquery,
  covered TableOrSubquery
)
RETURNS TableOrSubquery AS (
  WITH stacks AS (
    SELECT DISTINCT slice_id AS id FROM $blocked WHERE slice_id IS NOT NULL
    UNION
    SELECT a.id
    FROM (SELECT DISTINCT slice_id FROM $blocked WHERE slice_id IS NOT NULL) b
    JOIN ancestor_slice(b.slice_id) a
  )
  SELECT
    s.id,
    s.parent_id,
    s.name,
    s.id AS slice_id,
    cast(s.id AS TEXT) AS slice_ids,
    t.utid,
    t.upid,
    FALSE AS is_stitch,
    'slice' AS kind,
    0 AS self_dur,
    0 AS self_count
  FROM stacks
  JOIN slice s USING (id)
  LEFT JOIN _stitched_track t USING (track_id)
  UNION ALL
  SELECT * FROM _stitched_cp_thread_nodes!($pieces)
  UNION ALL
  SELECT * FROM _stitched_cp_blocker_nodes!($pieces, $groups, $covered)
);
`,
};
