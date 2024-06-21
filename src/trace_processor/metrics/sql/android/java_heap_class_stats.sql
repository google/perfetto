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
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--

SELECT RUN_METRIC('android/process_metadata.sql');

INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;
INCLUDE PERFETTO MODULE graphs.partition;

CREATE OR REPLACE PERFETTO FUNCTION _partition_tree_super_root_fn()
-- The assigned id of the "super root".
RETURNS INT AS
SELECT id + 1
FROM heap_graph_object
ORDER BY id DESC
LIMIT 1;

DROP TABLE IF EXISTS _heap_graph_dominator_tree_for_partition;
CREATE PERFETTO TABLE _heap_graph_dominator_tree_for_partition AS
SELECT
  tree.id,
  IFNULL(tree.idom_id, _partition_tree_super_root_fn()) as parent_id,
  obj.type_id as group_key
FROM heap_graph_dominator_tree tree
JOIN heap_graph_object obj USING(id)
UNION ALL
-- provide a single root required by tree partition if heap graph exists.
SELECT
  _partition_tree_super_root_fn() AS id,
  NULL AS parent_id,
  (SELECT id + 1 FROM heap_graph_class ORDER BY id desc LIMIT 1) AS group_key
WHERE _partition_tree_super_root_fn() IS NOT NULL;

DROP TABLE IF EXISTS _heap_object_marked_for_dominated_stats;
CREATE PERFETTO TABLE _heap_object_marked_for_dominated_stats AS
SELECT
  id,
  IIF(parent_id IS NULL, 1, 0) as marked
FROM tree_structural_partition_by_group!(_heap_graph_dominator_tree_for_partition)
ORDER BY id;

DROP TABLE IF EXISTS _heap_class_stats;
CREATE PERFETTO TABLE _heap_class_stats AS
SELECT
  obj.upid,
  obj.graph_sample_ts,
  obj.type_id,
  COUNT(1) AS obj_count,
  SUM(self_size) AS size_bytes,
  SUM(native_size) AS native_size_bytes,
  SUM(IIF(obj.reachable, 1, 0)) AS reachable_obj_count,
  SUM(IIF(obj.reachable, self_size, 0)) AS reachable_size_bytes,
  SUM(IIF(obj.reachable, native_size, 0)) AS reachable_native_size_bytes,
  SUM(IIF(marked, dominated_obj_count, 0)) AS dominated_obj_count,
  SUM(IIF(marked, dominated_size_bytes, 0)) AS dominated_size_bytes,
  SUM(IIF(marked, dominated_native_size_bytes, 0)) AS dominated_native_size_bytes
FROM heap_graph_object obj
-- Left joins to preserve unreachable objects.
LEFT JOIN _heap_object_marked_for_dominated_stats USING(id)
LEFT JOIN heap_graph_dominator_tree USING(id)
GROUP BY 1, 2, 3
ORDER BY 1, 2, 3;

DROP VIEW IF EXISTS java_heap_class_stats_output;
CREATE PERFETTO VIEW java_heap_class_stats_output AS
WITH
-- Group by to build the repeated field by upid, ts
heap_class_stats_count_protos AS (
  SELECT
    upid,
    graph_sample_ts,
    RepeatedField(JavaHeapClassStats_TypeCount(
      'type_name', IFNULL(c.deobfuscated_name, c.name),
      'obj_count', obj_count,
      'size_bytes', size_bytes,
      'native_size_bytes', native_size_bytes,
      'reachable_obj_count', reachable_obj_count,
      'reachable_size_bytes', reachable_size_bytes,
      'reachable_native_size_bytes', reachable_native_size_bytes,
      'dominated_obj_count', dominated_obj_count,
      'dominated_size_bytes', dominated_size_bytes,
      'dominated_native_size_bytes', dominated_native_size_bytes
    )) AS count_protos
  FROM _heap_class_stats s
  JOIN heap_graph_class c ON s.type_id = c.id
  GROUP BY 1, 2
),
-- Group by to build the repeated field by upid
heap_class_stats_sample_protos AS (
  SELECT
    upid,
    RepeatedField(JavaHeapClassStats_Sample(
      'ts', graph_sample_ts,
      'type_count', count_protos
    )) AS sample_protos
  FROM heap_class_stats_count_protos
  GROUP BY 1
)
SELECT JavaHeapClassStats(
  'instance_stats', RepeatedField(JavaHeapClassStats_InstanceStats(
    'upid', upid,
    'process', process_metadata.metadata,
    'samples', sample_protos
  )))
FROM heap_class_stats_sample_protos JOIN process_metadata USING (upid);
