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

INCLUDE PERFETTO MODULE android.memory.heap_graph.excluded_refs;
INCLUDE PERFETTO MODULE android.memory.heap_graph.helpers;
INCLUDE PERFETTO MODULE graphs.search;

-- Converts the heap graph into a tree by performing a BFS on the graph from
-- the roots. This basically ends up with all paths being the shortest path
-- from the root to the node (with lower ids being picked in the case of ties).
CREATE PERFETTO TABLE _heap_graph_object_min_depth_tree AS
SELECT node_id AS id, parent_node_id AS parent_id
FROM graph_reachable_bfs!(
  (
    SELECT owner_id AS source_node_id, owned_id AS dest_node_id
    FROM heap_graph_reference ref
    WHERE ref.id NOT IN _excluded_refs AND ref.owned_id IS NOT NULL
    ORDER BY ref.owned_id
  ),
  (
    SELECT id AS node_id
    FROM heap_graph_object
    WHERE root_type IS NOT NULL
  )
)
ORDER BY id;

CREATE PERFETTO TABLE _heap_graph_path_hashes as
SELECT *
FROM _heap_graph_type_path_hash!(_heap_graph_object_min_depth_tree);

CREATE PERFETTO TABLE _heap_graph_path_hashes_aggregated as
SELECT *
FROM _heap_graph_path_hash_aggregate!(_heap_graph_path_hashes);

CREATE PERFETTO TABLE _heap_graph_class_tree AS
SELECT *
FROM _heap_graph_path_hashes_to_class_tree!(_heap_graph_path_hashes_aggregated);
