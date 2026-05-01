--
-- Copyright 2026 The Android Open Source Project
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

INCLUDE PERFETTO MODULE std.trees.table_conversion;

-- Emit a serialized pprof Profile proto from a TREE pointer produced by
-- the `std.trees.*` operators (`_tree_from_table`, `_tree_filter`,
-- `_tree_propagate_down`, ...).
--
-- The pointer must carry the columns named by `name_col` (TEXT, frame
-- label) and `value_col` (INTEGER, self contribution). Rows whose value
-- is NULL or non-positive emit no Sample but remain available as
-- ancestors of deeper rows.
--
-- Returns a BLOB of raw (uncompressed) Profile proto bytes.
--
-- Simple recipe — wrap any (id, parent_id, ...) table:
--
--   SELECT _pprof_from_tree!(
--     _tree_from_table!(
--       (SELECT id, parent_id, name, dur AS value
--        FROM slice WHERE dur > 0),
--       (name, value)
--     ),
--     name, value, 'wall', 'nanoseconds'
--   );
--
-- Composable recipe — drop slices below 1ms then emit pprof:
--
--   SELECT _pprof_from_tree!(
--     _tree_filter(
--       _tree_from_table!(
--         (SELECT id, parent_id, name, dur AS value FROM slice),
--         (name, value)
--       ),
--       _tree_where(_tree_constraint('value', '>', 1000000))
--     ),
--     name, value, 'wall', 'nanoseconds'
--   );
--
-- For callers that don't want to take a dependency on `std.trees.*`,
-- the underlying `profile_from_tree(id, parent_id, name, value,
-- sample_type, unit)` aggregate is callable directly.
CREATE PERFETTO MACRO _pprof_from_tree(
  -- TREE pointer produced by an operator in `std.trees.*`.
  tree_ptr Expr,
  -- Frame-name column inside the tree pointer.
  name_col ColumnName,
  -- Value column inside the tree pointer.
  value_col ColumnName,
  -- Sample type label: appears in pprof's sample_type[0].type.
  sample_type Expr,
  -- Sample unit: appears in pprof's sample_type[0].unit (e.g. 'bytes').
  unit Expr
)
RETURNS Expr
AS (
  SELECT
    profile_from_tree(id, parent_id, $name_col, $value_col, $sample_type, $unit)
  FROM _tree_to_table!($tree_ptr, ($name_col, $value_col))
);
