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

-- Given a list of classes as ancestor classes, return all the classes that
-- descend from them.
--
-- The subclass spanning tree from the seed ancestors over the
-- superclass->subclass forest. Each descendant inherits the seed ancestor at the
-- root of its path via TREE ACCUMULATE DOWN FIRST(...) — what the old `_graph_scan!`
-- propagated down by hand. (A class reachable from several seeds is deduped to its
-- nearest seed ancestor, the natural spanning-tree semantics.)
CREATE PERFETTO MACRO android_heap_graph_class_find_descendants(
  -- ancestor class `id`s from the heap_graph_class table containing a
  -- single column: `id`
  ancestor_class_ids TableOrSubquery
)
-- Table of the schema
-- (id JOINID(heap_graph_class.id), ancestor_class_id JOINID(heap_graph_class.id), ancestor_class_name STRING)
-- id: `id` of the class as in heap_graph_class
-- ancestor_class_id: `id` of the ancestor class as given in the input
-- ancestor_class_name: `name` of the ancestor class as in heap_graph_class
RETURNS Pipeline
AS (
  GRAPH BFS TREE
    NODES (FROM heap_graph_class |> SELECT id AS node_id, name)
    EDGES (
      FROM heap_graph_class
      |> WHERE superclass_id IS NOT NULL
      |> SELECT superclass_id AS source_node_id, id AS dest_node_id
    )
    FROM (FROM $ancestor_class_ids |> SELECT id AS node_id)
  |> TREE ACCUMULATE DOWN
       FIRST(node_id) AS ancestor_class_id,
       FIRST(name) AS ancestor_class_name
  |> SELECT node_id AS id, ancestor_class_id, ancestor_class_name
);
