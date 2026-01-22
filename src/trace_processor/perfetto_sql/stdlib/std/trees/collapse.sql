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

-- Collapses parent-child chains where both nodes have the same key.
--
-- When a node has the same key as its parent, it is removed from the tree
-- and its children are reparented to the parent. This effectively merges
-- consecutive nodes with the same key into a single node.
--
-- Unlike tree_merge_siblings! which merges sibling nodes, this operation
-- merges along the parent-child axis. This is useful for creating "class
-- trees" from object trees where consecutive objects of the same type
-- should be collapsed.
--
-- The collapsed node keeps the chain root's column values (the first node
-- in the chain, which is the ancestor).
--
-- Example: Given a tree A(X) -> B(X) -> C(Y) where X and Y are keys,
-- after collapse: A(X) -> C(Y) (B is collapsed into A since both have key X)
--
-- This is a lazy operation: it stores the operation parameters and only
-- executes when tree_to_table! is called.
--
-- Example usage:
-- ```
-- SELECT * FROM tree_to_table!(
--   tree_collapse!(
--     tree_from_table!(input, id, parent_id, (name, value)),
--     tree_key!(name)
--   ),
--   (name, value)
-- );
-- ```
CREATE PERFETTO MACRO tree_collapse(
    -- A TREE pointer from tree_from_table! or a previous tree operation.
    tree Expr,
    -- A TREE_KEYS spec from tree_key!() specifying the column to compare.
    -- Note: Only single-key collapse is supported currently.
    key Expr
)
-- Returns a new TREE with the collapse operation queued for execution.
RETURNS Expr AS
tree_collapse($tree, $key);
