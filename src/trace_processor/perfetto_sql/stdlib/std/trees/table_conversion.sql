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

-- Helper macro to generate 'col_name', t.col_name pairs for tree_agg.
CREATE PERFETTO MACRO _tree_agg_col(col ColumnName)
RETURNS _ProjectionFragment AS __intrinsic_stringify!($col), t.$col;

-- Helper macro to bind a column in __intrinsic_table_ptr.
CREATE PERFETTO MACRO _tree_bind(c ColumnName, col ColumnName)
RETURNS Expr AS __intrinsic_table_ptr_bind(result.$c, __intrinsic_stringify!($col));

-- Helper macro to select a column with alias.
CREATE PERFETTO MACRO _tree_select(c ColumnName, col ColumnName)
RETURNS _ProjectionFragment AS result.$c AS $col;

-- Creates a tree structure from a table with id, parent_id, and additional columns.
--
-- The source table must have columns named 'id' and 'parent_id'.
--
-- Example usage:
-- ```
-- SELECT *
-- FROM tree_to_table!(
--   tree_from_table!(
--     (SELECT id, parent_id, name, value FROM my_table),
--     (name, value)
--   ),
--   (name, value)
-- );
-- ```
CREATE PERFETTO MACRO tree_from_table(
    -- A table/view/subquery containing the tree data.
    -- Must have columns 'id' and 'parent_id'.
    source_table TableOrSubquery,
    -- Additional columns to pass through (parenthesized, comma-separated).
    columns ColumnNameList
)
-- Returns a TREE pointer that can be used with tree_to_table! or other
-- tree operations.
RETURNS Expr AS
(
  SELECT __intrinsic_tree_agg(
    'id', t.id,
    'parent_id', t.parent_id,
    __intrinsic_token_apply!(_tree_agg_col, $columns)
  )
  FROM $source_table AS t
);

-- Converts a tree structure back to a table.
--
-- Output column order: _tree_id, _tree_parent_id, id, parent_id, then additional columns.
--
-- Example usage:
-- ```
-- SELECT *
-- FROM tree_to_table!(
--   tree_from_table!(
--     (SELECT id, parent_id, name FROM my_table),
--     (name)
--   ),
--   (name)
-- );
-- ```
CREATE PERFETTO MACRO tree_to_table(
    -- A TREE pointer, typically from tree_from_table! or a tree operation.
    tree_ptr Expr,
    -- Additional columns that were passed through.
    columns ColumnNameList
)
-- The returned table has (_tree_id, _tree_parent_id, id, parent_id, additional columns).
RETURNS TableOrSubquery AS
(
  SELECT
    result.c0 AS _tree_id,
    result.c1 AS _tree_parent_id,
    result.c2 AS id,
    result.c3 AS parent_id,
    __intrinsic_token_apply!(
      _tree_select,
      (c4, c5, c6, c7, c8, c9, c10, c11, c12, c13),
      $columns
    )
  FROM __intrinsic_table_ptr(__intrinsic_tree_to_table($tree_ptr)) result
  WHERE
    __intrinsic_table_ptr_bind(result.c0, '_tree_id')
    AND __intrinsic_table_ptr_bind(result.c1, '_tree_parent_id')
    AND __intrinsic_table_ptr_bind(result.c2, 'id')
    AND __intrinsic_table_ptr_bind(result.c3, 'parent_id')
    AND __intrinsic_token_apply_and!(
      _tree_bind,
      (c4, c5, c6, c7, c8, c9, c10, c11, c12, c13),
      $columns
    )
);
