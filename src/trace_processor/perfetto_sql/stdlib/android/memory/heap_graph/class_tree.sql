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

-- Converts the heap graph into a tree by a breadth first search from the
-- roots, so every object's path from a root is a shortest one. Where several
-- are, the roots are taken in order of class name, which keeps the tree
-- comparable across heap dumps, and then each object's references in order of
-- the object referred to. Also holds the cumulative size, native size and count
-- of each object: the totals over it and everything below it in the tree. Only
-- reachable objects are in the tree.
CREATE PERFETTO TABLE _heap_graph_object_min_depth_tree AS
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
  cumulative_size,
  cumulative_native_size,
  cumulative_count
|> ORDER BY id;

CREATE PERFETTO TABLE _heap_graph_path_hashes AS
SELECT * FROM _heap_graph_type_path_hash!(_heap_graph_object_min_depth_tree);

CREATE PERFETTO TABLE _heap_graph_path_hashes_aggregated AS
SELECT * FROM _heap_graph_path_hash_aggregate!(_heap_graph_path_hashes);

CREATE PERFETTO TABLE _heap_graph_class_tree AS
SELECT *
FROM _heap_graph_path_hashes_to_class_tree!(_heap_graph_path_hashes_aggregated);

CREATE PERFETTO MACRO _heap_graph_duplicate_objects_agg(
  path_hashes TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT
    count(DISTINCT path_hash) AS path_count,
    count() AS object_count,
    sum(o.self_size) AS total_size,
    sum(o.native_size) AS total_native_size,
    coalesce(c.deobfuscated_name, c.name) AS class_name
  FROM $path_hashes AS h
  JOIN heap_graph_object AS o
    ON h.id = o.id
  JOIN heap_graph_class AS c
    ON o.type_id = c.id
  GROUP BY
    class_name
  ORDER BY
    path_count DESC
);
