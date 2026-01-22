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
CREATE PERFETTO MACRO _tree_emit_bind(c ColumnName, col ColumnName)
RETURNS Expr AS __intrinsic_table_ptr_bind(result.$c, __intrinsic_stringify!($col));

-- Helper macro to select user columns from the result table.
CREATE PERFETTO MACRO _tree_emit_select(c ColumnName, col ColumnName)
RETURNS _ProjectionFragment AS result.$c AS $col;

-- Materializes a TREE as a table with structural columns (__node_id, __parent_id,
-- __depth) and user columns.
--
-- The user_columns must match exactly both the names and order of the columns
-- specified when building the tree with tree_from_table!.
--
-- Example usage:
-- ```
-- SELECT * FROM tree_to_table!(
--   tree_from_table!(input, id, parent_id, (name, ts, dur)),
--   (name, ts, dur)
-- );
-- ```
CREATE PERFETTO MACRO tree_to_table(
  -- A TREE pointer returned by tree_from_table! or other tree operations.
  tree Expr,
  -- A parenthesized, comma-separated list of user column names.
  -- Must match the columns used in tree_from_table!.
  user_columns ColumnNameList
)
-- Returns a table with __node_id, __parent_id, __depth, and user columns.
RETURNS TableOrSubquery AS
(
  SELECT
    result.c0 AS __node_id,
    result.c1 AS __parent_id,
    result.c2 AS __depth,
    __intrinsic_token_apply!(
      _tree_emit_select,
      (c3, c4, c5, c6, c7, c8, c9),
      $user_columns
    )
  FROM __intrinsic_table_ptr(__intrinsic_tree_emit($tree)) result
  WHERE
    __intrinsic_table_ptr_bind(result.c0, '__node_id')
    AND __intrinsic_table_ptr_bind(result.c1, '__parent_id')
    AND __intrinsic_table_ptr_bind(result.c2, '__depth')
    AND __intrinsic_token_apply_and!(
      _tree_emit_bind,
      (c3, c4, c5, c6, c7, c8, c9),
      $user_columns
    )
);
