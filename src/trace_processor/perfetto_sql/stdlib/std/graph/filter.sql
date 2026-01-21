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

-- Filters edges from a GRAPH where the specified boolean column is true.
--
-- The filter operation is lazy: it stores the operation parameters and only
-- executes when graph_to_tree!, graph_to_node_table!, or graph_to_edge_table!
-- is called.
--
-- Example usage (filter out excluded edges):
-- ```
-- SELECT * FROM graph_to_tree!(
--   graph_filter!(
--     graph_from_table!(edges, nodes, (excluded), (size)),
--     excluded
--   ),
--   roots,
--   BFS
-- );
-- ```
CREATE PERFETTO MACRO graph_filter(
    -- A GRAPH pointer from graph_from_table! or a previous graph operation.
    graph Expr,
    -- The name of a boolean column in the edge data. Edges where this column
    -- is true (non-zero) will be removed.
    column ColumnName
)
-- Returns a new GRAPH with the filter operation queued for execution.
RETURNS Expr AS
__intrinsic_graph_filter($graph, __intrinsic_stringify!($column));
