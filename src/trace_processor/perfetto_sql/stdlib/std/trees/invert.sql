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

-- Inverts a tree structure, making leaves become roots and vice versa.
--
-- This operation is used for bottom-up views (e.g., bottom-up flamegraphs)
-- where you want to see callers of functions rather than callees.
--
-- The algorithm:
-- 1. Original leaves become new roots
-- 2. Original parents become children of their former children
-- 3. Nodes appearing in multiple paths are merged by key
--
-- NOTE: The `original_id` and `original_parent_id` columns are nulled out
-- after this operation. This is because nodes can be duplicated across paths
-- and then merged, making original IDs meaningless. The output `__node_id`
-- and `__parent_id` columns contain fresh sequential IDs (0, 1, 2, ...).
--
-- This is a lazy operation: it stores the parameters and only
-- executes when tree_to_table! is called.
--
-- Example: Create bottom-up view of a call tree:
-- ```
-- SELECT * FROM tree_to_table!(
--   tree_invert!(
--     tree_from_table!(input, id, parent_id, (name, value)),
--     tree_key!(name),
--     tree_order!(value),
--     tree_agg!(value, SUM)
--   ),
--   (name, value)
-- );
-- ```
CREATE PERFETTO MACRO tree_invert(
    -- A TREE pointer from tree_from_table! or a previous tree operation.
    tree Expr,
    -- A TREE_KEY from tree_key!() specifying the merge key column.
    key Expr,
    -- A TREE_ORDER from tree_order!() specifying the order column.
    ord Expr,
    -- A TREE_AGG from tree_agg!() for column aggregation.
    agg1 Expr
)
-- Returns a new TREE with the invert operation queued for execution.
RETURNS Expr AS
tree_invert($tree, $key, $ord, $agg1);
