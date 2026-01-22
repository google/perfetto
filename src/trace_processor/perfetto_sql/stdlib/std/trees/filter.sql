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

-- Creates a TREE_DELETE_SPEC for use with tree_delete_node!.
-- Specifies which nodes to delete based on a column comparison.
--
-- Comparison operators:
-- - EQ: Equal (exact match)
-- - GLOB: Glob pattern match (for string columns)
CREATE PERFETTO MACRO tree_delete_spec(
    -- The column name to compare.
    col ColumnName,
    -- The comparison operator: EQ or GLOB.
    op ColumnName,
    -- The value to compare against.
    val Expr
)
RETURNS Expr AS
__intrinsic_tree_delete_spec(__intrinsic_stringify!($col), __intrinsic_stringify!($op), $val);

-- Deletes nodes from a TREE that match the given specification.
--
-- When a node is deleted, its children are reparented to the deleted node's
-- parent (or become roots if the deleted node was a root).
--
-- The delete operation is lazy: it stores the operation parameters and only
-- executes when tree_to_table! is called.
--
-- Example usage (delete all nodes with name='idle'):
-- ```
-- SELECT * FROM tree_to_table!(
--   tree_delete_node!(
--     tree_from_table!(input, id, parent_id, (name, ts, dur)),
--     tree_delete_spec!(name, EQ, 'idle')
--   ),
--   (name, ts, dur)
-- );
-- ```
--
-- Example usage (delete all nodes matching a glob pattern):
-- ```
-- SELECT * FROM tree_to_table!(
--   tree_delete_node!(
--     tree_from_table!(input, id, parent_id, (name, ts, dur)),
--     tree_delete_spec!(name, GLOB, '*internal*')
--   ),
--   (name, ts, dur)
-- );
-- ```
CREATE PERFETTO MACRO tree_delete_node(
    -- A TREE pointer from tree_from_table! or a previous tree operation.
    tree Expr,
    -- A TREE_DELETE_SPEC from tree_delete_spec!() specifying which nodes to delete.
    spec Expr
)
-- Returns a new TREE with the delete operation queued for execution.
RETURNS Expr AS
__intrinsic_tree_delete_node($tree, $spec);
