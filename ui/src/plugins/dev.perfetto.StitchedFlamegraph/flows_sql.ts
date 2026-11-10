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

// Downstream: everything the selected slices ran or triggered, following
// slice nesting and stitches, restricted to the selected time window.
//
// Used as three steps, each materialized by the caller:
//   reach = _stitched_down_reach!(seeds, start, end)
//   items = _stitched_down_items!(reach, start, end)
//   nodes = _stitched_down_nodes!(items)
export const STITCHED_FLOWS_DOWN_MODULE: StitchedSqlModule = {
  name: 'flows_down',
  deps: [STITCHED_CORE_MODULE],
  sql: `
-- Parent -> child edges of the stitched slice graph (nesting and stitches)
-- with the time span of the child, so that a selection can cheaply keep only
-- the edges into slices overlapping its window.
CREATE PERFETTO TABLE _stitched_down_edge(
  source_node_id LONG,
  dest_node_id LONG,
  dst_ts TIMESTAMP,
  dst_end TIMESTAMP
) AS
WITH bounds AS (SELECT end_ts FROM trace_bounds)
SELECT
  parent_id AS source_node_id,
  id AS dest_node_id,
  ts AS dst_ts,
  iif(dur < 0, (SELECT end_ts FROM bounds), ts + dur) AS dst_end
FROM slice
WHERE parent_id IS NOT NULL
UNION ALL
SELECT
  sp.parent_id AS source_node_id,
  sp.id AS dest_node_id,
  s.ts AS dst_ts,
  iif(s.dur < 0, (SELECT end_ts FROM bounds), s.ts + s.dur) AS dst_end
FROM _stitched_parent sp
JOIN slice s USING (id);

-- Slices ("id") reachable from the $seeds slices (a table with an "id"
-- column) through slices overlapping [$start_ts, $end_ts).
CREATE PERFETTO MACRO _stitched_down_reach(
  seeds TableOrSubquery,
  start_ts Expr,
  end_ts Expr
)
RETURNS TableOrSubquery AS (
  SELECT node_id AS id
  FROM graph_reachable_dfs!(
    (
      SELECT source_node_id, dest_node_id
      FROM _stitched_down_edge
      WHERE dst_ts < $end_ts AND dst_end > $start_ts
    ),
    (SELECT id AS node_id FROM $seeds)
  )
);

-- The $reach slices with their candidate parents and their time clipped to
-- [$start_ts, $end_ts).
CREATE PERFETTO MACRO _stitched_down_items(
  reach TableOrSubquery,
  start_ts Expr,
  end_ts Expr
)
RETURNS TableOrSubquery AS (
  SELECT
    s.id,
    s.name,
    s.parent_id AS track_parent,
    sp.parent_id AS stitch_parent,
    t.utid,
    t.upid,
    min(iif(s.dur < 0, $end_ts, s.ts + s.dur), $end_ts)
      - max(s.ts, $start_ts) AS cdur
  FROM $reach r
  JOIN slice s USING (id)
  LEFT JOIN _stitched_parent sp USING (id)
  LEFT JOIN _stitched_track t USING (track_id)
);

-- Stitched nodes of the $items (a table indexed by "id"): a node reached
-- both by nesting and by a stitch hangs below the stitch. Weights are the
-- clipped time minus the time of children running in place of the slice
-- (nested slices and the reply of a synchronous binder call).
CREATE PERFETTO MACRO _stitched_down_nodes(items TableOrSubquery)
RETURNS TableOrSubquery AS (
  WITH
    child_dur AS (
      SELECT track_parent AS id, cdur
      FROM $items
      WHERE track_parent IS NOT NULL
      UNION ALL
      SELECT c.stitch_parent AS id, c.cdur
      FROM $items c
      JOIN $items p ON p.id = c.stitch_parent
      WHERE c.name = 'binder reply' AND p.name = 'binder transaction'
    ),
    child_sum AS (
      SELECT id, sum(cdur) AS dur
      FROM child_dur
      GROUP BY id
    )
  SELECT
    i.id,
    coalesce(sp.id, tp.id) AS parent_id,
    i.name,
    i.id AS slice_id,
    cast(i.id AS TEXT) AS slice_ids,
    i.utid,
    i.upid,
    sp.id IS NOT NULL AS is_stitch,
    'slice' AS kind,
    max(i.cdur - coalesce(cs.dur, 0), 0) AS self_dur,
    1 AS self_count
  FROM $items i
  LEFT JOIN $items sp ON sp.id = i.stitch_parent
  LEFT JOIN $items tp ON tp.id = i.track_parent
  LEFT JOIN child_sum cs ON cs.id = i.id
);
`,
};

// Upstream: the chain of slices that caused the selected slices to run,
// across threads and processes, unbounded in time.
//
// Used as three steps, each materialized by the caller:
//   seeds = _stitched_up_seeds!(seed_ids, start, end)
//   reach = _stitched_up_reach!(seeds)
//   nodes = _stitched_up_nodes!(reach, seeds)
export const STITCHED_FLOWS_UP_MODULE: StitchedSqlModule = {
  name: 'flows_up',
  deps: [STITCHED_CORE_MODULE],
  sql: `
-- Child -> causal parent edges (the stitch parent if any, else the enclosing
-- slice), restricted to the slices an upward walk can leave its seeds
-- through: stitched slices plus the stitch sources and their ancestors. The
-- seeds' own ancestors are seeds too (same track, overlapping the window) so
-- the walk only ever leaves them through a stitch.
CREATE PERFETTO TABLE _stitched_up_edge(
  source_node_id LONG,
  dest_node_id LONG
) AS
WITH
  sources AS (
    SELECT DISTINCT parent_id AS id FROM _stitched_parent
  ),
  nodes AS (
    SELECT id FROM sources
    UNION
    SELECT a.id FROM sources s JOIN ancestor_slice(s.id) a
    UNION
    SELECT id FROM _stitched_parent
  )
SELECT
  n.id AS source_node_id,
  coalesce(sp.parent_id, s.parent_id) AS dest_node_id
FROM nodes n
JOIN slice s USING (id)
LEFT JOIN _stitched_parent sp USING (id)
WHERE coalesce(sp.parent_id, s.parent_id) IS NOT NULL;

-- The $seed_ids slices (a table with an "id" column) with their time
-- clipped to [$start_ts, $end_ts).
CREATE PERFETTO MACRO _stitched_up_seeds(
  seed_ids TableOrSubquery,
  start_ts Expr,
  end_ts Expr
)
RETURNS TableOrSubquery AS (
  SELECT
    s.id,
    s.parent_id,
    min(iif(s.dur < 0, $end_ts, s.ts + s.dur), $end_ts)
      - max(s.ts, $start_ts) AS cdur
  FROM $seed_ids x
  JOIN slice s USING (id)
);

-- The $seeds and every slice that causally led to them.
CREATE PERFETTO MACRO _stitched_up_reach(seeds TableOrSubquery)
RETURNS TableOrSubquery AS (
  SELECT node_id AS id
  FROM graph_reachable_dfs!(
    _stitched_up_edge,
    (SELECT id AS node_id FROM $seeds)
  )
);

-- Stitched nodes of the $reach slices. Only $seeds (a table indexed by "id")
-- carry weight: their clipped time minus the time of their nested seeds.
CREATE PERFETTO MACRO _stitched_up_nodes(
  reach TableOrSubquery,
  seeds TableOrSubquery
)
RETURNS TableOrSubquery AS (
  WITH seed_child AS (
    SELECT parent_id AS id, sum(cdur) AS dur
    FROM $seeds
    WHERE parent_id IS NOT NULL
    GROUP BY parent_id
  )
  SELECT
    s.id,
    coalesce(sp.parent_id, s.parent_id) AS parent_id,
    s.name,
    s.id AS slice_id,
    cast(s.id AS TEXT) AS slice_ids,
    t.utid,
    t.upid,
    sp.parent_id IS NOT NULL AS is_stitch,
    'slice' AS kind,
    iif(sd.id IS NULL, 0, max(sd.cdur - coalesce(sc.dur, 0), 0)) AS self_dur,
    sd.id IS NOT NULL AS self_count
  FROM $reach r
  JOIN slice s USING (id)
  LEFT JOIN _stitched_parent sp USING (id)
  LEFT JOIN _stitched_track t USING (track_id)
  LEFT JOIN $seeds sd ON sd.id = s.id
  LEFT JOIN seed_child sc ON sc.id = s.id
);
`,
};
