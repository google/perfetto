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

-- Helper macro to generate the dataframe aggregation arguments for user columns.
CREATE PERFETTO MACRO _tree_df_agg(col ColumnName)
RETURNS _ProjectionFragment AS __intrinsic_stringify!($col), source.$col;

-- Builds a TREE from a table with id, parent_id, and additional data columns.
--
-- A TREE is an opaque pointer that can be passed to tree_merge_siblings!,
-- tree_delete_node!, tree_propagate_up!, tree_propagate_down!, tree_invert!,
-- or tree_to_table!. It stores the tree structure efficiently using vectors for
-- structural data and a dataframe for passthrough user columns.
--
-- The input id and parent_id columns are stored as passthrough columns named
-- `original_id` and `original_parent_id`. After merge or invert operations,
-- these columns are nulled out since they become meaningless.
--
-- Example usage:
-- ```
-- SELECT * FROM tree_to_table!(
--   tree_from_table!(
--     (SELECT id, parent_id, name, ts, dur FROM stack_data),
--     id,
--     parent_id,
--     (name, ts, dur)
--   ),
--   (name, ts, dur)
-- );
-- ```
CREATE PERFETTO MACRO tree_from_table(
  -- A table/view/subquery containing the tree data. Must have columns for
  -- node ID, parent ID, and all columns specified in user_columns.
  source TableOrSubquery,
  -- The column containing the unique node identifier (must be integer).
  id_col ColumnName,
  -- The column containing the parent's node ID (integer, NULL for roots).
  parent_id_col ColumnName,
  -- A parenthesized, comma-separated list of additional columns to capture
  -- for merge operations. Example: (name, ts, dur).
  user_columns ColumnNameList
)
-- Returns a TREE pointer for use with tree operations.
RETURNS Expr AS
(
  SELECT __intrinsic_tree_from_parent_agg(
    source.$id_col,
    source.$parent_id_col
    __intrinsic_token_apply_prefix!(
      _tree_df_agg,
      $user_columns
    )
  )
  FROM $source AS source
);
