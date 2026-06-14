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

-- DELETED: graph_reachable_dfs / graph_reachable_bfs absorbed by
-- GRAPH DFS TREE / GRAPH BFS TREE.

-- Computes the next sibling node in a directed graph. The next node under a parent node
-- is determined by on the |sort_key|, which should be unique for every node under a parent.
-- The order of the next sibling is undefined if the |sort_key| is not unique.
--
-- Example usage:
-- ```
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
-- The returned table has the schema (node_id LONG, next_node_id LONG).
-- |node_id| is the id of the node from the input graph and |next_node_id|
-- is the id of the node which is its next sibling.
RETURNS TableOrSubquery
AS (
  SELECT
    node_id,
    lead(node_id) OVER (PARTITION BY node_parent_id ORDER BY sort_key) AS next_node_id
  FROM $graph_table
);

-- Computes the "reachable" set of nodes in a directed graph from a set of
-- starting (root) nodes by performing a depth-first search from each root node on the graph.
-- The search is bounded by the sum of edge weights on the path and the root node specifies the
-- max weight (inclusive) allowed before stopping the search.
-- The returned nodes are structured as a tree with parent-child relationships corresponding
-- to the order in which nodes were encountered by the DFS. Each row also has the root node from
-- which where the edge was encountered.
--
-- While this macro can be used directly by end users (hence being public),
-- it is primarily intended as a lower-level building block upon which higher
-- level functions/macros in the standard library can be built.
--
-- Example usage on traces with sched info:
-- ```
-- -- Compute the reachable nodes from a sched wakeup chain
-- INCLUDE PERFETTO MODULE sched.thread_executing_spans;
--
-- SELECT *
-- FROM
--   graph_reachable_dfs_bounded
--    !(
--      (
--        SELECT
--          id AS source_node_id,
--          COALESCE(parent_id, id) AS dest_node_id,
--          id - COALESCE(parent_id, id) AS edge_weight
--        FROM _wakeup_chain
--      ),
--      (
--        SELECT
--          id AS root_node_id,
--          id - COALESCE(prev_id, id) AS root_target_weight
--        FROM _wakeup_chain
--      ));
-- ```
-- DELETED: graph_reachable_weight_bounded_dfs == GRAPH DFS TREE (§7). The plain
-- (all-zero-weight) reachability is `GRAPH DFS TREE NODES/EDGES/FROM`; the seed
-- each node was reached under (root_node_id) is `TREE ACCUMULATE DOWN FIRST(node_id)`
-- (see slices/flow.sql). The weight-bounded cutoff is intentionally dropped.
