-- Copyright (C) 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--      http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Graph operators (docs/graphs.md). Native kernel: pf_graph_tree_agg (src/graphs.cc).
--
--   GRAPH DFS TREE       NODES n EDGES e FROM s  ->  pf_graph_dfs_tree('n', 'e', 's' [, per := [...]])
--   GRAPH BFS TREE       NODES n EDGES e FROM s  ->  pf_graph_bfs_tree('n', 'e', 's' [, per := [...]])
--   GRAPH DOMINATOR TREE NODES n EDGES e FROM s  ->  pf_graph_dominator_tree('n', 'e', 's' [, per := [...]])
--
-- n: table with column id (+ payload), e: table with columns src, dst,
-- s: table with column id. per: partition column names present in all three
-- (e.g. ['_trace_uuid'] or ['_trace_uuid', 'upid']); each partition is an
-- independent graph. Output: per cols, id, parent_id, depth, node payload.
-- Payload columns named parent_id / depth are replaced. Columns starting with
-- _pf_ are reserved.
CREATE OR REPLACE MACRO pf_graph_dfs_tree(nodes, edges, seeds, per := []::VARCHAR[], full_row := true) AS TABLE
FROM query(pf__sql_graph_tree('dfs', nodes, edges, seeds, per, full_row, current_setting('threads')::BIGINT))
-- @@
CREATE OR REPLACE MACRO pf_graph_bfs_tree(nodes, edges, seeds, per := []::VARCHAR[], full_row := true) AS TABLE
FROM query(pf__sql_graph_tree('bfs', nodes, edges, seeds, per, full_row, current_setting('threads')::BIGINT))
-- @@
CREATE OR REPLACE MACRO pf_graph_dominator_tree(nodes, edges, seeds, per := []::VARCHAR[], full_row := true) AS TABLE
FROM query(pf__sql_graph_tree('dominator', nodes, edges, seeds, per, full_row, current_setting('threads')::BIGINT))
