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

-- sqlformat file off
-- The case sensitivity of this file matters so don't format it which
-- changes sensitivity.

INCLUDE PERFETTO MODULE graphs.scan;

CREATE PERFETTO MACRO _viz_flamegraph_hash_coalesce(col ColumnName)
RETURNS Expr AS IFNULL($col, 0);

-- For each frame in |tab|, returns a row containing the result of running
-- all the filtering operations over that frame's name.
CREATE PERFETTO MACRO _viz_flamegraph_prepare_filter(
  tab TableOrSubquery,
  show_stack Expr,
  hide_stack Expr,
  show_from_frame Expr,
  hide_frame Expr,
  pivot Expr,
  impossible_stack_bits Expr,
  grouping ColumnNameList
)
RETURNS TableOrSubquery
AS (
  SELECT
    *,
    IIF($hide_stack, $impossible_stack_bits, $show_stack) AS stackBits,
    $show_from_frame As showFromFrameBits,
    $hide_frame = 0 AS showFrame,
    $pivot AS isPivot,
    HASH(
      name,
      __intrinsic_token_apply!(_viz_flamegraph_hash_coalesce, $grouping)
    ) AS groupingHash
  FROM $tab
  ORDER BY id
);

-- Walks the forest from root to leaf and performs the following operations:
--  1) removes frames which were filtered out
--  2) make any pivot nodes become the roots
--  3) computes whether the stack as a whole should be retained or not
CREATE PERFETTO MACRO _viz_flamegraph_filter_frames(
  source TableOrSubquery,
  show_from_frame_bits Expr
)
RETURNS TableOrSubquery
AS (
  WITH edges AS (
    SELECT parentId AS source_node_id, id AS dest_node_id
    FROM $source
    WHERE parentId IS NOT NULL
  ),
  inits AS (
    SELECT
      id,
      IIF(
        showFrame AND showFromFrameBits = $show_from_frame_bits,
        id,
        NULL
      ) AS filteredId,
      NULL AS filteredParentId,
      NULL AS filteredUnpivotedParentId,
      IIF(
        showFrame,
        showFromFrameBits,
        0
      ) AS showFromFrameBits,
      IIF(
        showFrame AND showFromFrameBits = $show_from_frame_bits,
        stackBits,
        0
      ) AS stackBits
    FROM $source
    WHERE parentId IS NULL
  )
  SELECT
    g.filteredId AS id,
    g.filteredParentId AS parentId,
    g.filteredUnpivotedParentId AS unpivotedParentId,
    g.stackBits,
    SUM(t.value) AS value
  FROM _graph_scan!(
    edges,
    inits,
    (filteredId, filteredParentId, filteredUnpivotedParentId, showFromFrameBits, stackBits),
    (
      SELECT
        t.id,
        IIF(
          x.showFrame AND (t.showFromFrameBits | x.showFromFrameBits) = $show_from_frame_bits,
          t.id,
          t.filteredId
        ) AS filteredId,
        IIF(
          x.showFrame AND (t.showFromFrameBits | x.showFromFrameBits) = $show_from_frame_bits,
          IIF(x.isPivot, NULL, t.filteredId),
          t.filteredParentId
        ) AS filteredParentId,
        IIF(
          x.showFrame AND (t.showFromFrameBits | x.showFromFrameBits) = $show_from_frame_bits,
          t.filteredId,
          t.filteredParentId
        ) AS filteredUnpivotedParentId,
        IIF(
          x.showFrame,
          (t.showFromFrameBits | x.showFromFrameBits),
          t.showFromFrameBits
        ) AS showFromFrameBits,
        IIF(
          x.showFrame AND (t.showFromFrameBits | x.showFromFrameBits) = $show_from_frame_bits,
          (t.stackBits | x.stackBits),
          t.stackBits
        ) AS stackBits
      FROM $table t
      JOIN $source x USING (id)
    )
  ) g
  JOIN $source t USING (id)
  WHERE filteredId IS NOT NULL
  GROUP BY filteredId
  ORDER BY filteredId
);

-- Walks the forest from leaves to root and does the following:
--   1) removes nodes whose stacks are filtered out
--   2) computes the cumulative value for each node (i.e. the sum of the self
--      value of the node and all descendants).
CREATE PERFETTO MACRO _viz_flamegraph_accumulate(
  filtered TableOrSubquery,
  showStackBits Expr
)
RETURNS TableOrSubquery
AS (
  WITH edges AS (
    SELECT id AS source_node_id, parentId AS dest_node_id
    FROM $filtered
    WHERE parentId IS NOT NULL
  ), inits AS (
    SELECT f.id, f.value AS cumulativeValue
    FROM $filtered f
    LEFT JOIN $filtered c ON c.parentId = f.id
    WHERE c.id IS NULL AND f.stackBits = $showStackBits
  )
  SELECT id, cumulativeValue
  FROM _graph_aggregating_scan!(
    edges,
    inits,
    (cumulativeValue),
    (
      SELECT
        x.id,
        x.childValue + IIF(
          t.stackBits = $showStackBits,
          t.value,
          0
        ) AS cumulativeValue
      FROM (
        SELECT id, SUM(cumulativeValue) AS childValue
        FROM $table
        GROUP BY id
      ) x
      JOIN $filtered t USING (id)
    )
  )
  ORDER BY id
);

CREATE PERFETTO MACRO _viz_flamegraph_s_prefix(col ColumnName)
RETURNS Expr AS s.$col;

-- Propagates the cumulative value of the pivot nodes to the roots and
-- computes the "fingerprint" of the path. Output is intentionally narrow:
-- it omits name and the |grouping| columns. Those are recovered later in
-- _viz_flamegraph_resolve_groups via a single per-merged-row JOIN against
-- |source|, which is far cheaper than the per-walk-row JOIN we'd pay if
-- we pulled them in here (the walk produces ~4x more rows than the merged
-- table). |grouped| columns ARE pulled in here because they're aggregated
-- across the whole hash group and would otherwise need a second source
-- JOIN over all walk rows just to sum them.
CREATE PERFETTO MACRO _viz_flamegraph_upwards_hash(
  source TableOrSubquery,
  filtered TableOrSubquery,
  accumulated TableOrSubquery,
  grouped ColumnNameList
)
RETURNS TableOrSubquery
AS (
  WITH edges AS (
    SELECT id AS source_node_id, unpivotedParentId AS dest_node_id
    FROM $filtered
    WHERE unpivotedParentId IS NOT NULL
  ),
  inits AS (
    SELECT
      f.id,
      HASH(-1, s.groupingHash) AS hash,
      NULL AS parentHash,
      -1 AS depth,
      a.cumulativeValue
    FROM $filtered f
    JOIN $source s USING (id)
    JOIN $accumulated a USING (id)
    WHERE s.isPivot AND a.cumulativeValue > 0
  )
  SELECT
    g.id,
    g.hash,
    g.parentHash,
    g.depth,
    __intrinsic_token_apply!(_viz_flamegraph_s_prefix, $grouped),
    f.value,
    g.cumulativeValue
  FROM _graph_scan!(
    edges,
    inits,
    (hash, parentHash, depth, cumulativeValue),
    (
      SELECT
        t.id,
        HASH(t.hash, x.groupingHash) AS hash,
        t.hash AS parentHash,
        t.depth - 1 AS depth,
        t.cumulativeValue
      FROM $table t
      JOIN $source x USING (id)
    )
  ) g
  JOIN $source s USING (id)
  JOIN $filtered f USING (id)
);

-- Computes the "fingerprint" of the path by walking from the roots to
-- the leaves. Output mirrors _viz_flamegraph_upwards_hash (skinny - no
-- name or |grouping| columns) for the same performance reasons.
CREATE PERFETTO MACRO _viz_flamegraph_downwards_hash(
  source TableOrSubquery,
  filtered TableOrSubquery,
  accumulated TableOrSubquery,
  grouped ColumnNameList,
  showDownward Expr
)
RETURNS TableOrSubquery
AS (
  WITH
    edges AS (
      SELECT parentId AS source_node_id, id AS dest_node_id
      FROM $filtered
      WHERE parentId IS NOT NULL
    ),
    inits AS (
      SELECT
        f.id,
        HASH(1, s.groupingHash) AS hash,
        NULL AS parentHash,
        1 AS depth
      FROM $filtered f
      JOIN $source s USING (id)
      WHERE f.parentId IS NULL AND $showDownward
    )
  SELECT
    g.id,
    g.hash,
    g.parentHash,
    g.depth,
    __intrinsic_token_apply!(_viz_flamegraph_s_prefix, $grouped),
    f.value,
    a.cumulativeValue
  FROM _graph_scan!(
    edges,
    inits,
    (hash, parentHash, depth),
    (
      SELECT
        t.id,
        HASH(t.hash, x.groupingHash) AS hash,
        t.hash AS parentHash,
        t.depth + 1 AS depth
      FROM $table t
      JOIN $source x USING (id)
    )
  ) g
  JOIN $source s USING (id)
  JOIN $filtered f USING (id)
  JOIN $accumulated a USING (id)
);

CREATE PERFETTO MACRO _col_list_id(a ColumnName)
RETURNS Expr AS $a;

CREATE PERFETTO MACRO _col_list_null(a ColumnName)
RETURNS _ProjectionFragment AS NULL AS $a;

CREATE PERFETTO MACRO _viz_flamegraph_g_prefix(col ColumnName)
RETURNS Expr AS g.$col;

-- Groups the hash table by hash, summing values and aggregating any
-- |grouped_agged_exprs| (e.g. GROUP_CONCAT) across all walk rows in the
-- group. Records MIN(c.id) as |rep_id| - any id with this hash will
-- have the same |grouping| columns in source, so a single representative
-- is sufficient for the _resolve_groups JOIN.
--
-- Caller MUST materialize this into a Perfetto table with an index on
-- |hash| - _viz_flamegraph_resolve_groups self-joins on hash to compute
-- parentId and that lookup must be O(log N).
CREATE PERFETTO MACRO _viz_flamegraph_group_hashes(
  hashed TableOrSubquery,
  grouped_agged_exprs ColumnNameList
)
RETURNS TableOrSubquery
AS (
  SELECT
    _auto_id AS id,
    MIN(c.id) AS rep_id,
    c.hash,
    MIN(c.parentHash) AS parentHash,
    c.depth,
    __intrinsic_token_apply!(_col_list_id, $grouped_agged_exprs),
    SUM(c.value) AS value,
    SUM(c.cumulativeValue) AS cumulativeValue
  FROM $hashed c
  GROUP BY c.hash
);

-- Resolves a |grouped| table (output of _viz_flamegraph_group_hashes) into
-- the merged tree. Self-joins on hash to compute parentId, and joins
-- |source| on rep_id to recover name and the |grouping| columns. This
-- replaces the per-walk-row source JOIN that the old single-pass
-- _merge_hashes did - 4x fewer JOIN rows, since |grouped| has one row
-- per unique hash rather than one per walk visit.
--
-- The parent's cumulativeValue is also carried through as
-- |parentCumulativeValue|. It piggybacks on the LEFT JOIN already used
-- for parentId, so it's essentially free here and lets
-- _viz_flamegraph_global_layout avoid a LEFT JOIN of its own.
--
-- |grouped_cols| are pass-through references to the columns that were
-- aggregated by _viz_flamegraph_group_hashes (their names, not the agg
-- expressions).
CREATE PERFETTO MACRO _viz_flamegraph_resolve_groups(
  grouped TableOrSubquery,
  source TableOrSubquery,
  grouping ColumnNameList,
  grouped_cols ColumnNameList
)
RETURNS TableOrSubquery
AS (
  SELECT
    g.id,
    p.id AS parentId,
    g.depth,
    s.name,
    __intrinsic_token_apply!(_viz_flamegraph_s_prefix, $grouping),
    __intrinsic_token_apply!(_viz_flamegraph_g_prefix, $grouped_cols),
    g.value,
    g.cumulativeValue,
    p.cumulativeValue AS parentCumulativeValue,
    FALSE AS isPlaceholder
  FROM $grouped g
  LEFT JOIN $grouped p ON p.hash = g.parentHash
  JOIN $source s ON s.id = g.rep_id
  -- Order by id so the resulting Perfetto table is dense in id-order;
  -- downstream graph_aggregating_scan over (parentId, id) is sensitive
  -- to non-sequential id storage (~5x slower without).
  ORDER BY g.id
);

-- Per-node propagation pass for the trim. For each node visited, emits:
--   alive       - whether the node survives the join thresholds.
--   requiredCum - the cumulativeValue this node's CHILDREN must clear.
-- Roots are always alive (seeded with requiredCum = 0). A node whose
-- cumulativeValue < parent's requiredCum emits requiredCum = +inf, which
-- kills its whole subtree on the next step.
--
-- The floor threshold |min_value| is applied by pre-filtering scan
-- edges: only dest nodes whose cumulativeValue clears the floor are
-- propagated. Sub-floor subtrees are naturally dead because scan
-- output excludes unreachable nodes, which is what alive_set reads.
-- This is ~12x faster than keeping the floor check inside the scan on
-- large trees because the scan input shrinks proportionally.
--
-- Caller should materialize the result into a Perfetto table with an
-- index on |id|, since _viz_flamegraph_trim_with_placeholder looks up
-- parentId -> alive while emitting placeholders.
CREATE PERFETTO MACRO _viz_flamegraph_trim_propagation(
  merged TableOrSubquery,
  ratio Expr,
  min_value Expr
)
RETURNS TableOrSubquery
AS (
  SELECT id, requiredCum, alive
  FROM _graph_aggregating_scan!(
    -- Edge filter is the floor: exclude dest rows below min_value.
    -- The scan can never reach them, so they end up implicitly dead.
    (
      SELECT m.parentId AS source_node_id, m.id AS dest_node_id
      FROM $merged m
      WHERE m.parentId IS NOT NULL
        AND m.cumulativeValue >= $min_value
    ),
    -- Roots stay alive regardless of floor (matches the prior semantic
    -- where the init always seeded TRUE).
    (
      SELECT id, 0.0 AS requiredCum, TRUE AS alive
      FROM $merged WHERE parentId IS NULL
    ),
    (requiredCum, alive),
    (
      SELECT
        x.id,
        IIF(
          t.cumulativeValue >= x.incoming,
          $ratio * t.cumulativeValue,
          1e308
        ) AS requiredCum,
        (t.cumulativeValue >= x.incoming) AS alive
      FROM (
        SELECT id, MIN(requiredCum) AS incoming
        FROM $table
        GROUP BY id
      ) x
      JOIN $merged t USING (id)
    )
  )
);

-- Returns the ids of nodes that survive the trim. Just a thin filter over
-- |propagated| now that |alive| is computed inside the propagation scan.
CREATE PERFETTO MACRO _viz_flamegraph_alive_set(
  propagated TableOrSubquery
)
RETURNS TableOrSubquery
AS (SELECT id FROM $propagated WHERE alive);

-- Emits the trimmed flamegraph: kept rows from |merged| pass through, plus
-- one synthetic '(merged)' placeholder per kept parent that has any
-- dropped direct children (split by depth-sign so a root with both
-- upward and downward dropped subtrees gets one placeholder per side).
--
-- |alive| must be a Perfetto table containing the kept ids (the output of
-- _viz_flamegraph_alive_set materialized with an index on id). Caller is
-- responsible for that materialization so the three references to |alive|
-- below all hit an indexed lookup.
CREATE PERFETTO MACRO _viz_flamegraph_trim_with_placeholder(
  merged TableOrSubquery,
  alive TableOrSubquery,
  grouping ColumnNameList,
  grouped ColumnNameList
)
RETURNS TableOrSubquery
AS (
  WITH _max_id AS (
    SELECT COALESCE(MAX(id), 0) AS v FROM $merged
  ),
  _unsorted AS (
    SELECT
      m.id, m.parentId, m.depth, m.name,
      __intrinsic_token_apply!(_col_list_id, $grouping),
      __intrinsic_token_apply!(_col_list_id, $grouped),
      m.value, m.cumulativeValue, m.parentCumulativeValue,
      FALSE AS isPlaceholder
    FROM $merged m
    JOIN $alive a USING (id)
    UNION ALL
    SELECT
      (SELECT v FROM _max_id)
        + ROW_NUMBER() OVER (ORDER BY d.parentId, (d.depth > 0)) AS id,
      d.parentId,
      -- All dropped children of one parent on one side of the root share
      -- the same depth in a tree, so MIN/MAX/ANY are equivalent.
      MIN(d.depth) AS depth,
      '(merged)' AS name,
      __intrinsic_token_apply!(_col_list_null, $grouping),
      __intrinsic_token_apply!(_col_list_null, $grouped),
      SUM(d.value) AS value,
      SUM(d.cumulativeValue) AS cumulativeValue,
      -- All dropped children of one parent share the same parent, so
      -- MIN is just "any" here - we use it to stay inside GROUP BY.
      MIN(d.parentCumulativeValue) AS parentCumulativeValue,
      TRUE AS isPlaceholder
    FROM $merged d
    LEFT JOIN $alive a ON a.id = d.id
    WHERE
      a.id IS NULL
      AND d.parentId IS NOT NULL
      AND d.parentId IN (SELECT id FROM $alive)
    -- A root node can have both upward and downward dropped subtrees;
    -- keep them as separate placeholders since they sit on opposite sides.
    GROUP BY d.parentId, (d.depth > 0)
  )
  -- Order by id so the resulting Perfetto table is dense in id-order;
  -- _viz_flamegraph_global_layout's graph_scan over (parentId, id) is
  -- sensitive to non-sequential id storage (~5x slower without).
  SELECT * FROM _unsorted ORDER BY id
);

-- Performs a "layout" of nodes in the flamegraph relative to their
-- siblings.
CREATE PERFETTO MACRO _viz_flamegraph_local_layout(
  merged TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  WITH partial_layout AS (
    SELECT
      id,
      cumulativeValue,
      SUM(cumulativeValue) OVER win AS xEnd
    FROM $merged
    WHERE cumulativeValue > 0
    WINDOW win AS (
      PARTITION BY parentId, depth
      ORDER BY cumulativeValue DESC
      ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
    )
  )
  SELECT id, xEnd - cumulativeValue as xStart, xEnd
  FROM partial_layout
  ORDER BY id
);

-- Walks the graph from root to leaf, propogating the layout of
-- parents to their children.
CREATE PERFETTO MACRO _viz_flamegraph_global_layout(
  merged TableOrSubquery,
  layout TableOrSubquery,
  grouping ColumnNameList,
  grouped ColumnNameList
)
RETURNS TableOrSubquery
AS (
  WITH edges AS (
    SELECT parentId AS source_node_id, id AS dest_node_id
    FROM $merged
    WHERE parentId IS NOT NULL
  ),
  inits AS (
    SELECT h.id, 1 AS rootDistance, l.xStart, l.xEnd
    FROM $merged h
    JOIN $layout l USING (id)
    WHERE h.parentId IS NULL
  )
  SELECT
    s.id,
    IFNULL(s.parentId, -1) AS parentId,
    IIF(s.name = '', 'unknown', s.name) AS name,
    __intrinsic_token_apply!(_viz_flamegraph_s_prefix, $grouping),
    __intrinsic_token_apply!(_viz_flamegraph_s_prefix, $grouped),
    s.value AS selfValue,
    s.cumulativeValue,
    s.parentCumulativeValue,
    s.depth,
    g.xStart,
    g.xEnd,
    s.isPlaceholder
  FROM _graph_scan!(
    edges,
    inits,
    (rootDistance, xStart, xEnd),
    (
      SELECT
        t.id,
        t.rootDistance + 1 as rootDistance,
        t.xStart + w.xStart AS xStart,
        t.xStart + w.xEnd AS xEnd
      FROM $table t
      JOIN $layout w USING (id)
    )
  ) g
  JOIN $merged s USING (id)
  ORDER BY rootDistance, xStart
);
