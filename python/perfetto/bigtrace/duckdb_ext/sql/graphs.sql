-- Graph operators (spec §7). Native kernel: pf_graph_tree_agg (src/graphs.cc).
--
--   GRAPH DFS TREE       NODES n EDGES e FROM s  ->  pf_graph_dfs_tree('n', 'e', 's' [, per := [...]])
--   GRAPH BFS TREE       NODES n EDGES e FROM s  ->  pf_graph_bfs_tree('n', 'e', 's' [, per := [...]])
--   GRAPH DOMINATOR TREE NODES n EDGES e FROM s  ->  pf_graph_dominator_tree('n', 'e', 's' [, per := [...]])
--
-- n: table with column id (+ payload), e: table with columns src, dst,
-- s: table with column id. per: partition column names present in all three
-- (e.g. ['trace_idx'] or ['trace_idx', 'upid']); each partition is an
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
