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
-- distributed under the License is distributed ON an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

INCLUDE PERFETTO MODULE android.memory.heap_graph.excluded_refs;

-- All reachable heap graph objects arranged into a shortest-path spanning tree
-- from GC roots via breadth-first search over strong references.
--
-- Every reachable object's path to a root via `parent_id` is a shortest
-- strong-reference path. Where multiple shortest paths exist, roots are ordered
-- by class name and `id` (keeping the tree comparable across heap dumps) and
-- outgoing references are ordered by target `id`. Only reachable objects appear
-- in this table.
CREATE PERFETTO TABLE heap_graph_shortest_path_tree(
  -- Heap graph object id.
  id JOINID(heap_graph_object.id),
  -- Parent object id on a shortest strong-reference path from a GC root, or
  -- NULL if this object is a GC root.
  parent_id JOINID(heap_graph_object.id),
  -- Count of all objects in this object's subtree in the shortest-path tree,
  -- self inclusive.
  cumulative_count LONG,
  -- Total self_size (in bytes) of all objects in this object's subtree in the
  -- shortest-path tree, self inclusive.
  cumulative_size LONG,
  -- Total native_size (in bytes) of all objects in this object's subtree in the
  -- shortest-path tree, self inclusive.
  cumulative_native_size LONG,
  -- Depth of the object in the shortest-path tree. Depth of GC root objects is
  -- 0.
  depth LONG
)
AS
GRAPH BFS TREE
  NODES heap_graph_object
  EDGES (
    FROM _heap_graph_strong_edges
    |> ORDER BY dst
  )
  FROM (
    SELECT o.id
    FROM heap_graph_object AS o
    JOIN heap_graph_class AS c ON o.type_id = c.id
    WHERE
      o.root_type IS NOT NULL
    ORDER BY
      c.name,
      o.id
  )
|> TREE ACCUMULATE UP
  SUM(self_size) AS cumulative_size,
  SUM(native_size) AS cumulative_native_size,
  COUNT(*) AS cumulative_count
|> SELECT
  id,
  parent_id,
  cumulative_count,
  cumulative_size,
  cumulative_native_size,
  root_distance AS depth
|> ORDER BY id;
