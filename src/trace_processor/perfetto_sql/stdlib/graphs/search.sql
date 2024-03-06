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

-- Computes the "reachable" set of nodes in a directed graph from a given
-- starting node by performing a depth-first search on the graph. The returned
-- nodes are structured as a tree with parent-child relationships corresponding
-- to the order in which nodes were encountered by the DFS.
--
-- While this macro can be used directly by end users (hence being public),
-- it is primarily intended as a lower-level building block upon which higher
-- level functions/macros in the standard library can be built.
--
-- Example usage on traces containing heap graphs:
--
-- -- Compute the reachable nodes from the first heap root.
-- SELECT *
-- FROM graph_reachable_dfs!(
--   (
--     SELECT
--       owner_id AS source_node_id,
--       owned_id as dest_node_id
--     FROM heap_graph_reference
--     WHERE owned_id IS NOT NULL
--   ),
--   (SELECT id FROM heap_graph_object WHERE root_type IS NOT NULL LIMIT 1)
-- );
-- ```
CREATE PERFETTO MACRO graph_reachable_dfs(
  -- A table/view/subquery corresponding to a directed graph on which the
  -- reachability search should be performed. This table must have the columns
  -- "source_node_id" and "dest_node_id" corresponding to the two nodes on
  -- either end of the edges in the graph.
  --
  -- Note: the columns must contain uint32 similar to ids in trace processor
  -- tables (i.e. the values should be relatively dense and close to zero). The
  -- implementation makes assumptions on this for performance reasons and, if
  -- this criteria is not, can lead to enormous amounts of memory being
  -- allocated.
  graph_table TableOrSubquery,
  -- The start node to |graph_table| which will be the root of the reachability
  -- tree.
  start_node_id Expr
)
-- The returned table has the schema (node_id UINT32, parent_node_id UINT32).
-- |node_id| is the id of the node from the input graph and |parent_node_id|
-- is the id of the node which was the first encountered predecessor in a DFS
-- search of the graph.
RETURNS TableOrSubquery AS
(
  WITH __temp_graph_table AS (SELECT * FROM $graph_table)
  SELECT dt.node_id, dt.parent_node_id
  FROM __intrinsic_dfs(
    (SELECT RepeatedField(source_node_id) FROM __temp_graph_table),
    (SELECT RepeatedField(dest_node_id) FROM __temp_graph_table),
    $start_node_id
  ) dt
);

-- Computes the next sibling node in a directed graph. The next node under a parent node
-- is determined by on the |sort_key|, which should be unique for every node under a parent.
-- The order of the next sibling is undefined if the |sort_key| is not unique.
--
-- Example usage:
--
-- -- Compute the next sibling:
-- SELECT *
-- FROM graph_next_sibling!(
--   (
--     SELECT
--       id AS node_id,
--       parent_id AS node_parent_id,
--       ts AS sort_key
--     FROM slice
--   )
-- );
-- ```
CREATE PERFETTO MACRO graph_next_sibling(
  -- A table/view/subquery corresponding to a directed graph for which to find the next sibling.
  -- This table must have the columns "node_id", "node_parent_id" and "sort_key".
  graph_table TableOrSubquery
)
-- The returned table has the schema (node_id UINT32, next_node_id UINT32).
-- |node_id| is the id of the node from the input graph and |next_node_id|
-- is the id of the node which is its next sibling.
RETURNS TableOrSubquery AS
(
  SELECT node_id, lead(node_id) OVER (PARTITION BY node_parent_id ORDER BY sort_key) AS next_node_id
    FROM $graph_table
);
