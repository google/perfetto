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

-- sqlformat file off

-- Propagates values down a tree from roots to leaves using BFS.
--
-- For each child node, combines the parent's value with the child's value
-- using the specified combine operation. This is useful for computing
-- cumulative values like depth, cumulative sums, or inherited properties.
--
-- Combine operations:
--   'sum':   child += parent (e.g., compute depth with initial values of 1)
--   'min':   child = min(parent, child)
--   'max':   child = max(parent, child)
--   'first': child = parent (parent's value propagates down, overwriting child)
--   'last':  no-op (child keeps its own value)
--
-- Example usage:
-- ```
-- SELECT * FROM _tree_to_table!(
--   _tree_propagate_down(
--     _tree_from_table!((SELECT id, parent_id, value FROM my_tree), (value)),
--     'value',
--     'sum',
--     'cumulative_value'
--   ),
--   (cumulative_value)
-- );
-- ```
CREATE PERFETTO FUNCTION _tree_propagate_down(
    -- A TREE pointer from _tree_from_table or another tree operation.
    tree_ptr ANY,
    -- Name of the column to propagate values for.
    column_name STRING,
    -- Combine operation: 'sum', 'min', 'max', 'first', or 'last'.
    combine_op STRING,
    -- Name of the output column containing propagated values.
    output_name STRING
)
-- Returns a TREE pointer with propagated values
RETURNS ANY
DELEGATES TO __intrinsic_tree_propagate_down;
