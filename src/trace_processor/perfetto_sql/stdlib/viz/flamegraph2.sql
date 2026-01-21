--
-- Copyright 2025 The Android Open Source Project
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

-- Flamegraph visualization using the tree algebra system.
--
-- This module provides macros for building flamegraph visualizations using
-- the lazy tree algebra operations (tree_from_table, tree_delete_node,
-- tree_merge_siblings, tree_propagate_up, tree_invert, etc.)

-- sqlformat file off

INCLUDE PERFETTO MODULE std.trees.filter;
INCLUDE PERFETTO MODULE std.trees.from_table;
INCLUDE PERFETTO MODULE std.trees.invert;
INCLUDE PERFETTO MODULE std.trees.merge;
INCLUDE PERFETTO MODULE std.trees.propagate;
INCLUDE PERFETTO MODULE std.trees.to_table;

-- Build a top-down flamegraph from a tree-structured input table.
--
-- This macro:
-- 1. Builds a tree from the input table
-- 2. Merges siblings with the same name (summing values)
-- 3. Computes cumulative values (sum of self + all descendants)
-- 4. Materializes the result as a table
--
-- Output columns:
-- - __node_id: The internal node ID (row index)
-- - __parent_id: The parent's node ID (NULL for roots)
-- - __depth: Depth in the tree (0 for roots)
-- - name: The frame name
-- - value: The self value (after merging)
-- - cumulative_value: The sum of value for this node and all descendants
--
-- Example usage:
-- ```
-- SELECT * FROM _viz_flamegraph_tree_topdown!(
--   (SELECT id, parent_id, name, dur AS value FROM stack_frames),
--   name,
--   value
-- );
-- ```
CREATE PERFETTO MACRO _viz_flamegraph_tree_topdown(
  -- Input table with id, parent_id, name_col, value_col columns.
  source TableOrSubquery,
  -- Column containing the frame name (used as merge key).
  name_col ColumnName,
  -- Column containing the self value to sum.
  value_col ColumnName
)
RETURNS TableOrSubquery
AS (
  SELECT * FROM tree_to_table!(
    tree_propagate_up!(
      tree_merge_siblings!(
        tree_from_table!(
          $source,
          id,
          parent_id,
          ($name_col, $value_col)
        ),
        tree_merge_mode!(GLOBAL),
        tree_key!($name_col),
        tree_order!($value_col),
        tree_agg!($value_col, SUM)
      ),
      tree_propagate_spec!(cumulative_value, $value_col, SUM)
    ),
    ($name_col, $value_col, cumulative_value)
  )
);

-- Build a bottom-up flamegraph from a tree-structured input table.
--
-- This macro inverts the tree so leaves become roots, then merges siblings
-- with the same name. This produces a "callers" view where you see which
-- functions called a given function.
--
-- Output columns are the same as _viz_flamegraph_tree_topdown.
--
-- Example usage:
-- ```
-- SELECT * FROM _viz_flamegraph_tree_bottomup!(
--   (SELECT id, parent_id, name, dur AS value FROM stack_frames),
--   name,
--   value
-- );
-- ```
CREATE PERFETTO MACRO _viz_flamegraph_tree_bottomup(
  -- Input table with id, parent_id, name_col, value_col columns.
  source TableOrSubquery,
  -- Column containing the frame name (used as merge key).
  name_col ColumnName,
  -- Column containing the self value to sum.
  value_col ColumnName
)
RETURNS TableOrSubquery
AS (
  SELECT * FROM tree_to_table!(
    tree_propagate_up!(
      tree_invert!(
        tree_from_table!(
          $source,
          id,
          parent_id,
          ($name_col, $value_col)
        ),
        tree_key!($name_col),
        tree_order!($value_col),
        tree_agg!($value_col, SUM)
      ),
      tree_propagate_spec!(cumulative_value, $value_col, SUM)
    ),
    ($name_col, $value_col, cumulative_value)
  )
);

-- Compute the local layout of flamegraph nodes within their siblings.
--
-- This adds xStart and xEnd columns based on the cumulative value,
-- ordering siblings by cumulative value descending.
--
-- Example usage:
-- ```
-- WITH fg AS (
--   SELECT * FROM _viz_flamegraph_tree_topdown!(source, name, value)
-- )
-- SELECT * FROM _viz_flamegraph_tree_local_layout!(fg);
-- ```
CREATE PERFETTO MACRO _viz_flamegraph_tree_local_layout(
  -- A table from _viz_flamegraph_tree_topdown or _viz_flamegraph_tree_bottomup.
  source TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT
    s.*,
    COALESCE(
      SUM(s.cumulative_value) OVER (
        PARTITION BY s.__parent_id
        ORDER BY s.cumulative_value DESC
        ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
      ),
      0
    ) AS xStart,
    SUM(s.cumulative_value) OVER (
      PARTITION BY s.__parent_id
      ORDER BY s.cumulative_value DESC
      ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
    ) AS xEnd
  FROM $source s
  WHERE s.cumulative_value > 0
);

-- Compute the global layout by propagating parent offsets to children.
--
-- This transforms local xStart/xEnd values into global coordinates.
-- Uses tree_propagate_down to accumulate the parent's xStart offset.
CREATE PERFETTO MACRO _viz_flamegraph_tree_global_layout(
  -- A table from _viz_flamegraph_tree_local_layout.
  source TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT * FROM tree_to_table!(
    tree_propagate_down!(
      tree_from_table!(
        $source,
        __node_id,
        __parent_id,
        (name, value, cumulative_value, xStart, xEnd, __depth)
      ),
      tree_propagate_spec!(global_xStart, xStart, SUM)
    ),
    (name, value, cumulative_value, xStart, xEnd, __depth, global_xStart)
  )
);
