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

-- Helper macro to bind user columns from the result table.
CREATE PERFETTO MACRO _graph_edge_emit_bind(c ColumnName, col ColumnName)
RETURNS Expr AS __intrinsic_table_ptr_bind(result.$c, __intrinsic_stringify!($col));

-- Helper macro to select user columns from the result table.
CREATE PERFETTO MACRO _graph_edge_emit_select(c ColumnName, col ColumnName)
RETURNS _ProjectionFragment AS result.$c AS $col;

-- Materializes graph edges as a table with source_id, dest_id, and user columns.
--
-- Any pending filter operations are applied before materializing.
--
-- Example usage:
-- ```
-- SELECT * FROM graph_to_edge_table!(
--   graph_filter!(
--     graph_from_table!(edges, nodes, (excluded, weight), (size)),
--     excluded
--   ),
--   (excluded, weight)
-- );
-- ```
CREATE PERFETTO MACRO graph_to_edge_table(
  -- A GRAPH pointer returned by graph_from_table! or other graph operations.
  graph Expr,
  -- A parenthesized, comma-separated list of user column names.
  -- Must match the edge columns used in graph_from_table!.
  user_columns ColumnNameList
)
-- Returns a table with source_id, dest_id, and user columns.
RETURNS TableOrSubquery AS
(
  SELECT
    result.c0 AS source_id,
    result.c1 AS dest_id
    __intrinsic_token_apply!(
      _graph_edge_emit_select,
      (c2, c3, c4, c5, c6, c7, c8),
      $user_columns
    )
  FROM __intrinsic_table_ptr(__intrinsic_graph_edge_emit($graph)) result
  WHERE
    __intrinsic_table_ptr_bind(result.c0, 'source_id')
    AND __intrinsic_table_ptr_bind(result.c1, 'dest_id')
    AND __intrinsic_token_apply_and!(
      _graph_edge_emit_bind,
      (c2, c3, c4, c5, c6, c7, c8),
      $user_columns
    )
);
