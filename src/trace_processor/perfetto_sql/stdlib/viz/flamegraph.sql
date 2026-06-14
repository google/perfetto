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

-- ILLUSTRATIVE prototype translation. The upstream flamegraph engine built the
-- graph by hand over `_graph_scan!` / `_graph_aggregating_scan!`: a root->leaf
-- filtering scan, a leaf->root accumulating scan, and ~100 lines of path-hashing
-- (`_upwards_hash` + `_downwards_hash` + `_merge_hashes`) to collapse identical
-- paths. Each of those is a single tree operator:
--   * the filtering scan          == TREE WHERE / PRUNE / CONTRACT (§6.2-6.3)
--   * the path hash + merge_hashes == TREE MERGE SIBLINGS BY name      (§6.5)
--   * the accumulating scan        == TREE ACCUMULATE UP SUM           (§6.8)
--   * the "pivot" caller view      == TREE INVERT                      (§6.7)
-- Layout (xStart/xEnd) is geometry, not a tree op, so it stays window functions.

-- The flamegraph fold. Takes a callstack forest (`id`, `parentId`, `name`, a
-- `value` self-weight, plus any grouping columns) with the focus/hide selections
-- already materialised as id sets, and returns the merged, accumulated flamegraph.
CREATE PERFETTO MACRO _viz_flamegraph(
  -- Callstack forest: id, parentId, name, value, + grouping columns.
  source TableOrSubquery,
  -- Frames to focus from: only their subtrees are kept (show_from_frame).
  show_from_frames TableOrSubquery,
  -- Frames whose entire stacks are removed (hide_stack).
  hide_stacks TableOrSubquery,
  -- Frames removed in place, their weight folded into the parent (hide_frame).
  hide_frames TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  FROM $source
  -- show_from_frame: restrict to the subtrees under the focus frames.
  |> TREE WHERE DESCENDANT OF $show_from_frames
  -- hide_stack: remove the hidden frames AND their subtrees entirely.
  |> TREE PRUNE AT $hide_stacks
  -- hide_frame: remove the frame, reparent its children, charge its self up.
  |> TREE CONTRACT AT $hide_frames AGGREGATE SUM(value) AS value
  -- The fold: re-key every node by its root-to-node name path and unify nodes
  -- with the same path. This single stage replaces the entire hash machinery.
  |> TREE MERGE SIBLINGS BY name AGGREGATE SUM(value) AS value
  -- Cumulative value = self + all descendants.
  |> TREE ACCUMULATE UP SUM(value) AS cumulativeValue
);

-- The inverted ("caller" / pivot) flamegraph: flip leaves and roots first, so the
-- fold groups by callee->caller path instead of caller->callee. This is the only
-- difference between the bottom-up and top-down views.
CREATE PERFETTO MACRO _viz_flamegraph_inverted(
  source TableOrSubquery,
  show_from_frames TableOrSubquery,
  hide_stacks TableOrSubquery,
  hide_frames TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  FROM $source
  |> TREE WHERE DESCENDANT OF $show_from_frames
  |> TREE PRUNE AT $hide_stacks
  |> TREE CONTRACT AT $hide_frames AGGREGATE SUM(value) AS value
  |> TREE INVERT
  |> TREE MERGE SIBLINGS BY name AGGREGATE SUM(value) AS value
  |> TREE ACCUMULATE UP SUM(value) AS cumulativeValue
);

-- Lay out each node's horizontal extent relative to its siblings, then propagate
-- parent offsets to children. Pure geometry over the merged tree; no tree
-- operator involved (the cumulative value it consumes came from the fold above).
CREATE PERFETTO MACRO _viz_flamegraph_layout(merged TableOrSubquery)
RETURNS TableOrSubquery
AS (
  WITH local AS (
    SELECT
      id,
      parentId,
      depth,
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
  SELECT id, xEnd - cumulativeValue AS xStart, xEnd
  FROM local
  ORDER BY id
);
