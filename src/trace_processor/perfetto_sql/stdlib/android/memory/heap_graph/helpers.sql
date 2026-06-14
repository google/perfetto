--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed ON an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Folds a "tree-ified" heap object table (id, parent_id — built by a dominator
-- tree or shortest-path tree) into a class tree. The hand-rolled path-hash
-- `_graph_scan!` machinery this replaces was three tree operations:
--   1. a node with the SAME type as its parent reused the parent's path hash
--      (`IIF(o.type_id = t.parent_type_id, t.path_hash, …)`) == TREE MERGE INTO
--      PARENT BY type_id — collapsing self-referential same-class chains
--      (linked lists, Node->Node->Node) into their parent;
--   2. `GROUP BY path_hash` == TREE MERGE SIBLINGS BY type_id — unifying nodes
--      sharing a root-to-node class path into one class-tree node;
--   3. the "[native] …" side-node carved out of each node's native size ==
--      TREE EXPAND DOWN … CHARGE native_size TO LEAF.
CREATE PERFETTO MACRO _heap_graph_fold_to_class_tree(object_tree TableOrSubquery)
RETURNS Pipeline
AS (
  FROM $object_tree AS t
  |> JOIN heap_graph_object AS o ON t.id = o.id
  |> JOIN heap_graph_class AS c ON o.type_id = c.id
  |> SELECT
       t.id,
       t.parent_id,
       o.graph_sample_ts,
       o.upid,
       o.type_id,
       coalesce(c.deobfuscated_name, c.name) AS name,
       o.root_type,
       o.heap_type,
       1 AS self_count,
       o.self_size AS self_size,
       o.native_size AS native_size
  -- (1) Collapse same-type-as-parent chains into their parent.
  |> TREE MERGE INTO PARENT BY type_id
       AGGREGATE
         SUM(self_count) AS self_count,
         SUM(self_size) AS self_size,
         SUM(native_size) AS native_size
  -- (2) Unify nodes sharing a root-to-node class path: the object tree becomes a
  -- class tree.
  |> TREE MERGE SIBLINGS BY type_id
       AGGREGATE
         SUM(self_count) AS self_count,
         SUM(self_size) AS self_size,
         SUM(native_size) AS native_size
  -- (3) Carve each node's native size into a synthetic "[native] <class>" child.
  |> FORK AS folded
  |> TREE EXPAND DOWN (
       FROM folded
       |> WHERE native_size > 0
       |> SELECT
            id AS node_id,
            '[native] ' || name AS name,
            'HEAP_TYPE_NATIVE' AS heap_type
     ) BY node_id CHARGE native_size TO LEAF
);
