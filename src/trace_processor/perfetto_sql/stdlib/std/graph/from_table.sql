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

-- sqlformat file off

-- Helper macro to generate the dataframe aggregation arguments for edge columns.
CREATE PERFETTO MACRO _graph_edge_df_agg(col ColumnName)
RETURNS _ProjectionFragment AS __intrinsic_stringify!($col), edges.$col;

-- Helper macro to generate the dataframe aggregation arguments for node columns.
CREATE PERFETTO MACRO _graph_node_df_agg(col ColumnName)
RETURNS _ProjectionFragment AS __intrinsic_stringify!($col), nodes.$col;

-- Builds a GRAPH from edge and node tables.
--
-- A GRAPH is an opaque pointer that can be passed to graph_filter!,
-- graph_to_tree!, graph_to_node_table!, or graph_to_edge_table!.
-- It stores the graph structure with separate edge and node data.
--
-- Example usage:
-- ```
-- SELECT * FROM graph_to_node_table!(
--   graph_from_table!(
--     (SELECT owner_id AS source_id, owned_id AS dest_id, excluded
--      FROM heap_graph_reference),
--     (SELECT id, self_size, type_id FROM heap_graph_object),
--     (excluded),
--     (self_size, type_id)
--   ),
--   (self_size, type_id)
-- );
-- ```
CREATE PERFETTO MACRO graph_from_table(
  -- A table/view/subquery containing the edge data. Must have columns
  -- source_id and dest_id (both integer), plus any additional edge columns.
  edges TableOrSubquery,
  -- A table/view/subquery containing the node data. Must have an id column
  -- (integer), plus any additional node columns.
  nodes TableOrSubquery,
  -- A parenthesized, comma-separated list of additional edge columns to capture.
  -- Example: (excluded, weight).
  edge_columns ColumnNameList,
  -- A parenthesized, comma-separated list of additional node columns to capture.
  -- Example: (size, type_id).
  node_columns ColumnNameList
)
-- Returns a GRAPH pointer for use with graph operations.
RETURNS Expr AS
__intrinsic_graph_build(
  (
    SELECT __intrinsic_graph_nodes_agg(
      nodes.id
      __intrinsic_token_apply_prefix!(
        _graph_node_df_agg,
        $node_columns
      )
    )
    FROM $nodes AS nodes
  ),
  (
    SELECT __intrinsic_graph_edges_agg(
      edges.source_id,
      edges.dest_id
      __intrinsic_token_apply_prefix!(
        _graph_edge_df_agg,
        $edge_columns
      )
    )
    FROM $edges AS edges
  )
);
