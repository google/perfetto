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

INCLUDE PERFETTO MODULE android.memory.heap_graph.helpers;

INCLUDE PERFETTO MODULE android.memory.heap_graph.shortest_path_tree;

-- Deprecated alias for `heap_graph_shortest_path_tree`, kept for existing users.
CREATE PERFETTO VIEW _heap_graph_object_min_depth_tree AS
SELECT id, parent_id, cumulative_size, cumulative_native_size, cumulative_count
FROM heap_graph_shortest_path_tree;

CREATE PERFETTO TABLE _heap_graph_path_hashes AS
SELECT * FROM _heap_graph_type_path_hash!(heap_graph_shortest_path_tree);

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
