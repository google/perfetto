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

-- Alternative implementation of class_tree using the new graph + tree operators.
--
-- This implementation uses graph_from_table!, graph_filter!, graph_to_tree!
-- and tree_merge_siblings! to build a class tree from the heap graph,
-- replacing the manual path hash computation with tree algebra operations.

-- INCLUDE PERFETTO MODULE android.memory.heap_graph.excluded_refs;
INCLUDE PERFETTO MODULE std.graph.from_table;
INCLUDE PERFETTO MODULE std.graph.filter;
INCLUDE PERFETTO MODULE std.graph.to_tree;
INCLUDE PERFETTO MODULE std.trees.collapse;
INCLUDE PERFETTO MODULE std.trees.merge;
INCLUDE PERFETTO MODULE std.trees.propagate;
INCLUDE PERFETTO MODULE std.trees.to_table;


-- Build the object tree using BFS from roots, filtering excluded edges.
-- This produces the same tree structure as _heap_graph_object_min_depth_tree.
-- Then:
-- 1. Collapse parent-child chains with the same type_id (like path hashing)
-- 2. Merge siblings with the same name
--
-- Simplified output - only essential columns for now.
-- TODO: Add back root_type, heap_type, graph_sample_ts, upid once tree algebra
-- supports more columns properly.
CREATE PERFETTO TABLE _heap_graph_class_tree2 AS
SELECT
  __node_id AS id,
  __parent_id AS parent_id,
  name,
  self_size
FROM tree_to_table!(
  tree_merge_siblings!(
    tree_collapse!(
      graph_to_tree!(
        graph_from_table!(
          (
            SELECT
              ref.owner_id AS source_id,
              ref.owned_id AS dest_id
            FROM heap_graph_reference ref
            WHERE ref.owned_id IS NOT NULL
              AND ref.field_name != 'java.lang.ref.Reference.referent'
            UNION ALL
            SELECT
              ref.owner_id AS source_id,
              ref.owned_id AS dest_id
            FROM heap_graph_reference ref
            JOIN heap_graph_object o ON ref.owner_id = o.id
            JOIN heap_graph_class c ON o.type_id = c.id
            WHERE ref.owned_id IS NOT NULL
              AND ref.field_name = 'java.lang.ref.Reference.referent'
              AND c.kind NOT IN (
                'KIND_FINALIZER_REFERENCE',
                'KIND_PHANTOM_REFERENCE',
                'KIND_SOFT_REFERENCE',
                'KIND_WEAK_REFERENCE'
              )
          ),
          (
            SELECT
              o.id,
              o.type_id,
              HASH(
                o.type_id,
                COALESCE(o.root_type, ''),
                COALESCE(o.heap_type, '')
              ) AS obj_hash,
              coalesce(c.deobfuscated_name, c.name) AS name,
              o.self_size
            FROM heap_graph_object o
            JOIN heap_graph_class c ON o.type_id = c.id
          ),
          (),
          (type_id, obj_hash, name, self_size)
        ),
        (
          SELECT o.id
          FROM heap_graph_object o
          JOIN heap_graph_class c ON o.type_id = c.id
          WHERE o.root_type IS NOT NULL
          ORDER BY c.name, o.id
        ),
        BFS
      ),
      tree_key!(type_id)
    ),
    tree_merge_mode!(GLOBAL),
    tree_key!(obj_hash),
    tree_order!(self_size),
    tree_agg!(self_size, SUM)
  ),
  (name, self_size)
);
