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

-- Converts a GRAPH to a TREE using BFS or DFS traversal from root nodes.
--
-- The resulting TREE can be used with tree algebra operations like
-- tree_merge!, tree_propagate_up!, tree_propagate_down!, tree_invert!,
-- and tree_to_table!.
--
-- For BFS: produces a shortest-path tree where each node's parent is the
-- node that discovered it first in BFS order.
--
-- For DFS: produces a DFS tree where each node's parent is the node that
-- discovered it first in DFS order.
--
-- Only nodes reachable from the root nodes are included in the output tree.
-- Node passthrough columns from graph_from_table! are carried through to
-- the tree.
--
-- Example usage:
-- ```
-- SELECT * FROM tree_to_table!(
--   tree_merge!(
--     graph_to_tree!(
--       graph_filter!(
--         graph_from_table!(edges, nodes, (excluded), (size, type_id)),
--         excluded
--       ),
--       (SELECT id FROM gc_roots),
--       BFS
--     ),
--     tree_strategy!(GLOBAL),
--     tree_key!(type_id),
--     tree_order!(size),
--     tree_agg!(size, SUM)
--   ),
--   (size, type_id)
-- );
-- ```
CREATE PERFETTO MACRO graph_to_tree(
    -- A GRAPH pointer from graph_from_table! or a previous graph operation.
    graph Expr,
    -- A table/view/subquery containing the root node IDs. Must have a column
    -- named 'id' containing the node IDs to start traversal from.
    roots TableOrSubquery,
    -- Traversal mode: BFS (breadth-first search) or DFS (depth-first search).
    -- BFS produces shortest-path trees, DFS produces DFS trees.
    mode ColumnName
)
-- Returns a TREE pointer for use with tree algebra operations.
RETURNS Expr AS
(
  SELECT
    __intrinsic_graph_to_tree_agg($graph, roots.id, __intrinsic_stringify!($mode))
  FROM $roots AS roots
);
