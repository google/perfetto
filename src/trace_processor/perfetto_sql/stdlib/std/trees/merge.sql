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

-- Creates a TREE_MERGE_STRATEGY spec for use with tree_merge_siblings!.
-- Specifies how siblings with the same key should be merged.
--
-- Merge modes:
-- - CONSECUTIVE: Only merge adjacent siblings with the same key
-- - GLOBAL: Merge all siblings with the same key regardless of position
CREATE PERFETTO MACRO tree_merge_mode(
    -- The merge mode: CONSECUTIVE or GLOBAL.
    mode ColumnName
)
RETURNS Expr AS
__intrinsic_tree_merge_strategy(__intrinsic_stringify!($mode));

-- Creates a TREE_KEY spec for use with tree_merge_siblings!.
-- The key column determines which siblings are merged together.
-- NOTE: Prefer tree_keys! for new code as it supports multiple keys.
CREATE PERFETTO MACRO tree_key(
    -- The column name to use as the merge key.
    col ColumnName
)
RETURNS Expr AS
__intrinsic_tree_keys(__intrinsic_stringify!($col));

-- Helper macro to stringify a single column name for tree_keys!.
CREATE PERFETTO MACRO _tree_key_stringify(
    col ColumnName
)
RETURNS Expr AS
__intrinsic_stringify!($col);

-- Creates a TREE_KEYS spec for use with tree_merge_siblings!.
-- Accepts multiple key columns - siblings are merged if ALL keys match.
--
-- Example: tree_keys!((col1, col2, col3))
CREATE PERFETTO MACRO tree_keys(
    -- A parenthesized list of column names to use as the composite merge key.
    cols ColumnNameList
)
RETURNS Expr AS
__intrinsic_tree_keys(__intrinsic_token_apply!(_tree_key_stringify, $cols));

-- Creates a TREE_ORDER spec for use with tree_merge_siblings!.
-- The order column determines the ordering of siblings before merging.
CREATE PERFETTO MACRO tree_order(
    -- The column name to use for ordering siblings.
    col ColumnName
)
RETURNS Expr AS
__intrinsic_tree_order(__intrinsic_stringify!($col));

-- Creates a TREE_AGG spec for use with tree_merge_siblings!.
-- Specifies how to aggregate a column when siblings are merged.
--
-- Aggregation types:
-- - MIN: Take the minimum value
-- - MAX: Take the maximum value
-- - SUM: Sum all values
-- - COUNT: Count the number of merged nodes
-- - ANY: Take any value (first encountered)
CREATE PERFETTO MACRO tree_agg(
    -- The column name to aggregate.
    col ColumnName,
    -- The aggregation type: MIN, MAX, SUM, COUNT, or ANY.
    agg_type ColumnName
)
RETURNS Expr AS
__intrinsic_tree_agg(__intrinsic_stringify!($col), __intrinsic_stringify!($agg_type));

-- Merges sibling nodes in a TREE that share the same key value(s).
--
-- This operation is useful for stack charts where consecutive function calls
-- with the same name should be merged, summing their durations.
--
-- Supports both single-key and composite-key merging:
-- - Single key: tree_key!(col) or tree_keys!((col))
-- - Multiple keys: tree_keys!((col1, col2, col3))
--
-- NOTE: The `original_id` and `original_parent_id` columns are nulled out
-- after this operation since merged nodes no longer have a single original ID.
--
-- The merge operation is lazy: it stores the operation parameters and only
-- executes when tree_to_table! is called.
--
-- Example usage (single key - merge consecutive siblings with same 'name'):
-- ```
-- SELECT * FROM tree_to_table!(
--   tree_merge_siblings!(
--     tree_from_table!(input, id, parent_id, (name, ts, dur)),
--     tree_merge_mode!(CONSECUTIVE),
--     tree_key!(name),
--     tree_order!(ts),
--     tree_agg!(dur, SUM)
--   ),
--   (name, ts, dur)
-- );
-- ```
--
-- Example usage (multiple keys - merge siblings with same type_id AND root_type):
-- ```
-- SELECT * FROM tree_to_table!(
--   tree_merge_siblings!(
--     tree,
--     tree_merge_mode!(GLOBAL),
--     tree_keys!((type_id, root_type, heap_type)),
--     tree_order!(self_size),
--     tree_agg!(self_size, SUM)
--   ),
--   (name, self_size)
-- );
-- ```
CREATE PERFETTO MACRO tree_merge_siblings(
    -- A TREE pointer from tree_from_table! or a previous tree_merge_siblings!.
    tree Expr,
    -- A TREE_MERGE_STRATEGY spec from tree_merge_mode!().
    mode Expr,
    -- A TREE_KEYS spec from tree_key!() or tree_keys!() specifying the
    -- column(s) to group by. Siblings are merged if ALL keys match.
    keys Expr,
    -- A TREE_ORDER spec from tree_order!() specifying sibling order.
    ord Expr,
    -- A TREE_AGG spec from tree_agg!() specifying how to aggregate a column.
    agg1 Expr
)
-- Returns a new TREE with the merge operation queued for execution.
RETURNS Expr AS
tree_merge_siblings($tree, $mode, $keys, $ord, $agg1);
