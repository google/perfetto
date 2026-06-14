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

INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;

INCLUDE PERFETTO MODULE graphs.partition;

CREATE PERFETTO FUNCTION _partition_tree_super_root_fn()
-- The assigned id of the "super root".
RETURNS LONG
AS
SELECT id + 1 FROM heap_graph_object ORDER BY id DESC LIMIT 1;

CREATE PERFETTO FUNCTION _is_libcore_or_array(obj_name STRING)
RETURNS BOOL
AS
SELECT
  ($obj_name GLOB 'java.*' AND NOT ($obj_name GLOB 'java.lang.Class<*>'))
  OR $obj_name GLOB 'j$.*'
  OR $obj_name GLOB 'int[[]*'
  OR $obj_name GLOB 'long[[]*'
  OR $obj_name GLOB 'byte[[]*'
  OR $obj_name GLOB 'char[[]*'
  OR $obj_name GLOB 'short[[]*'
  OR $obj_name GLOB 'float[[]*'
  OR $obj_name GLOB 'double[[]*'
  OR $obj_name GLOB 'boolean[[]*'
  OR $obj_name GLOB 'android.util.*Array*'
  OR $obj_name GLOB 'kotlinx.coroutines.*'
  OR $obj_name GLOB 'kotlinx.atomicfu.*';

-- The dominator tree keyed by the object's type, with the hand-built super-root
-- providing a single root for the partition.
CREATE PERFETTO PIPELINE _heap_graph_dominator_tree_for_partition MATERIALIZED AS
SUBPIPELINE super_root AS (
  FROM heap_graph_object
  |> WHERE _partition_tree_super_root_fn() IS NOT NULL
  |> AGGREGATE
     ANY_VALUE(_partition_tree_super_root_fn()) AS id,
     ANY_VALUE(NULL) AS parent_id,
     ANY_VALUE((SELECT id + 1 FROM heap_graph_class ORDER BY id DESC LIMIT 1)) AS group_key
)
FROM heap_graph_dominator_tree AS tree
|> JOIN heap_graph_object AS obj USING (id)
|> SELECT
  tree.id,
  coalesce(tree.idom_id, _partition_tree_super_root_fn()) AS parent_id,
  obj.type_id AS group_key
-- provide a single root required by tree partition if heap graph exists.
|> UNION ALL (FROM super_root);

-- Partitions the dominator tree by group_key (type), marking the root of each
-- partition (a node whose nearest ancestor lies in a different group).
CREATE PERFETTO PIPELINE _heap_object_marked_for_dominated_stats MATERIALIZED AS
-- The markers are the partition boundary nodes: a node whose parent disagrees
-- on group_key (and the true roots, whose parent is null).
SUBPIPELINE boundaries AS (
  FROM _heap_graph_dominator_tree_for_partition AS node
  |> LEFT JOIN _heap_graph_dominator_tree_for_partition AS parent
     ON node.parent_id = parent.id
  |> WHERE parent.id IS NULL OR parent.group_key != node.group_key
  |> SELECT node.id
)
FROM _heap_graph_dominator_tree_for_partition
|> TREE REPARENT TO NEAREST ANCESTOR IN boundaries
|> SELECT id, iif(parent_id IS NULL, 1, 0) AS marked
|> ORDER BY id;

-- Class-level breakdown of the java heap.
-- Per type name aggregates the object stats and the dominator tree stats.
CREATE PERFETTO PIPELINE android_heap_graph_class_aggregation(
  -- Process upid
  upid JOINID(process.id),
  -- Heap dump timestamp
  graph_sample_ts TIMESTAMP,
  -- Class type id
  type_id LONG,
  -- Class name (deobfuscated if available)
  type_name STRING,
  -- Is type an instance of a libcore object (java.*) or array
  is_libcore_or_array BOOL,
  -- Count of class instances
  obj_count LONG,
  -- Size of class instances
  size_bytes LONG,
  -- Native size of class instances
  native_size_bytes LONG,
  -- Count of reachable class instances
  reachable_obj_count LONG,
  -- Size of reachable class instances
  reachable_size_bytes LONG,
  -- Native size of reachable class instances
  reachable_native_size_bytes LONG,
  -- Count of all objects dominated by instances of this class
  -- Only applies to reachable objects
  dominated_obj_count LONG,
  -- Size of all objects dominated by instances of this class
  -- Only applies to reachable objects
  dominated_size_bytes LONG,
  -- Native size of all objects dominated by instances of this class
  -- Only applies to reachable objects
  dominated_native_size_bytes LONG
) AS
SUBPIPELINE base AS (
  -- First level aggregation to avoid joining with class for every object
  FROM heap_graph_object AS obj
  -- Left joins to preserve unreachable objects.
  |> LEFT JOIN _heap_object_marked_for_dominated_stats USING (id)
  |> LEFT JOIN heap_graph_dominator_tree USING (id)
  |> AGGREGATE
     count(1) AS obj_count,
     sum(self_size) AS size_bytes,
     sum(native_size) AS native_size_bytes,
     sum(iif(obj.reachable, 1, 0)) AS reachable_obj_count,
     sum(iif(obj.reachable, self_size, 0)) AS reachable_size_bytes,
     sum(iif(obj.reachable, native_size, 0)) AS reachable_native_size_bytes,
     sum(iif(marked, dominated_obj_count, 0)) AS dominated_obj_count,
     sum(iif(marked, dominated_size_bytes, 0)) AS dominated_size_bytes,
     sum(iif(marked, dominated_native_size_bytes, 0)) AS dominated_native_size_bytes
     GROUP BY obj.upid, obj.graph_sample_ts, obj.type_id
)
FROM base
|> JOIN heap_graph_class AS cls
   ON base.type_id = cls.id
|> AGGREGATE
   sum(obj_count) AS obj_count,
   sum(size_bytes) AS size_bytes,
   sum(native_size_bytes) AS native_size_bytes,
   sum(reachable_obj_count) AS reachable_obj_count,
   sum(reachable_size_bytes) AS reachable_size_bytes,
   sum(reachable_native_size_bytes) AS reachable_native_size_bytes,
   sum(dominated_obj_count) AS dominated_obj_count,
   sum(dominated_size_bytes) AS dominated_size_bytes,
   sum(dominated_native_size_bytes) AS dominated_native_size_bytes
   GROUP BY
     upid,
     graph_sample_ts,
     type_id,
     coalesce(cls.deobfuscated_name, cls.name) AS type_name,
     _is_libcore_or_array(coalesce(cls.deobfuscated_name, cls.name)) AS is_libcore_or_array
|> ORDER BY
   upid,
   graph_sample_ts,
   type_id,
   type_name,
   is_libcore_or_array;
