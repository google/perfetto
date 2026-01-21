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

-- Creates a TREE_PROPAGATE_SPEC for use with tree_propagate_up!.
-- Specifies how to aggregate values from children to parents.
--
-- Aggregation types:
-- - SUM: Sum of node's value and all children's aggregated values
-- - MIN: Minimum across node and children
-- - MAX: Maximum across node and children
-- - COUNT: Count of values (node + children)
CREATE PERFETTO MACRO tree_propagate_spec(
    -- The output column name to create with aggregated values.
    out_col ColumnName,
    -- The input column name to aggregate.
    in_col ColumnName,
    -- The aggregation type: SUM, MIN, MAX, COUNT.
    agg_type ColumnName
)
RETURNS Expr AS
__intrinsic_tree_propagate_spec(__intrinsic_stringify!($out_col), __intrinsic_stringify!($in_col), __intrinsic_stringify!($agg_type));

-- Propagates values from leaves to root using aggregation.
--
-- Each node's output value is the aggregation of its own input value
-- with all its children's output values. Processing happens in
-- reverse topological order (leaves first, then parents).
--
-- This is useful for computing cumulative values in tree structures,
-- such as cumulative time in flamegraphs where each node's cumulative
-- time is its own time plus all its children's cumulative times.
--
-- This is a lazy operation: it stores the parameters and only
-- executes when tree_to_table! is called.
--
-- Example: Compute cumulative sum of 'self_value' column:
-- ```
-- SELECT * FROM tree_to_table!(
--   tree_propagate_up!(
--     tree_from_table!(input, id, parent_id, (name, self_value)),
--     tree_propagate_spec!(cumulative, self_value, SUM)
--   ),
--   (name, self_value, cumulative)
-- );
-- ```
CREATE PERFETTO MACRO tree_propagate_up(
    -- A TREE pointer from tree_from_table! or a previous tree operation.
    tree Expr,
    -- A TREE_PROPAGATE_SPEC from tree_propagate_spec!().
    spec Expr
)
-- Returns a new TREE with the propagate-up operation queued for execution.
RETURNS Expr AS
__intrinsic_tree_propagate_up($tree, $spec);

-- Propagates values from root to leaves using aggregation.
--
-- Each node's output value is the aggregation of its parent's output value
-- with its own input value. Root nodes use their input value directly
-- (no parent contribution). Processing happens in topological order
-- (roots first, then children).
--
-- This is useful for computing cumulative path values or propagating
-- flags down the tree. For example, marking all descendants of a node
-- for deletion, or computing cumulative path lengths.
--
-- This is a lazy operation: it stores the parameters and only
-- executes when tree_to_table! is called.
--
-- Example: Compute cumulative path sum from root to each node:
-- ```
-- SELECT * FROM tree_to_table!(
--   tree_propagate_down!(
--     tree_from_table!(input, id, parent_id, (name, value)),
--     tree_propagate_spec!(path_sum, value, SUM)
--   ),
--   (name, value, path_sum)
-- );
-- ```
CREATE PERFETTO MACRO tree_propagate_down(
    -- A TREE pointer from tree_from_table! or a previous tree operation.
    tree Expr,
    -- A TREE_PROPAGATE_SPEC from tree_propagate_spec!().
    spec Expr
)
-- Returns a new TREE with the propagate-down operation queued for execution.
RETURNS Expr AS
__intrinsic_tree_propagate_down($tree, $spec);
