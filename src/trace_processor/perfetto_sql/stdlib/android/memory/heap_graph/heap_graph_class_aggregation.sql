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

CREATE PERFETTO FUNCTION _is_static_class(obj_name STRING)
RETURNS BOOL
AS
SELECT $obj_name GLOB 'java.lang.Class<*>';

-- Unwraps java.lang.Class<*> and normalizes the 'j$.' desugared prefix to 'java.'.
CREATE PERFETTO FUNCTION _normalize_java_class_name(obj_name STRING)
RETURNS STRING
AS
SELECT
  CASE
    WHEN $obj_name GLOB 'java.lang.Class<j$.*>' THEN 'java.'
    || substr($obj_name, 20, length($obj_name) - 20)
    WHEN _is_static_class($obj_name) THEN substr(
      $obj_name,
      17,
      length($obj_name) - 17
    )
    WHEN $obj_name GLOB 'j$.*' THEN 'java.' || substr($obj_name, 4)
    ELSE $obj_name
  END;

CREATE PERFETTO FUNCTION _is_language_runtime_normalized(cls_name STRING)
RETURNS BOOL
AS
SELECT
  $cls_name GLOB 'java.*'
  OR $cls_name GLOB 'kotlin.*'
  OR $cls_name GLOB 'kotlinx.*';

CREATE PERFETTO FUNCTION _is_container_normalized(cls_name STRING)
RETURNS BOOL
AS
SELECT
  $cls_name GLOB '*[[]]'
  OR $cls_name GLOB 'android.util.*Array*'
  OR $cls_name GLOB 'android.util.MapCollections*'
  OR $cls_name GLOB 'android.util.LruCache*'
  OR $cls_name GLOB 'kotlin.collections.*'
  OR $cls_name GLOB 'androidx.collection.*'
  OR $cls_name = 'java.util.Arrays$ArrayList'
  OR $cls_name GLOB 'java.util.Arrays$ArrayList$*'
  OR CASE str_split($cls_name, '$', 0)
    -- java.util concrete collection and map classes
    WHEN 'java.util.ArrayDeque' THEN 1
    WHEN 'java.util.ArrayList' THEN 1
    WHEN 'java.util.BitSet' THEN 1
    WHEN 'java.util.Collections' THEN 1
    WHEN 'java.util.EnumMap' THEN 1
    WHEN 'java.util.HashMap' THEN 1
    WHEN 'java.util.HashSet' THEN 1
    WHEN 'java.util.Hashtable' THEN 1
    WHEN 'java.util.IdentityHashMap' THEN 1
    WHEN 'java.util.ImmutableCollections' THEN 1
    WHEN 'java.util.JumboEnumSet' THEN 1
    WHEN 'java.util.LinkedHashMap' THEN 1
    WHEN 'java.util.LinkedHashSet' THEN 1
    WHEN 'java.util.LinkedList' THEN 1
    WHEN 'java.util.PriorityQueue' THEN 1
    WHEN 'java.util.Properties' THEN 1
    WHEN 'java.util.RegularEnumSet' THEN 1
    WHEN 'java.util.Stack' THEN 1
    WHEN 'java.util.TreeMap' THEN 1
    WHEN 'java.util.TreeSet' THEN 1
    WHEN 'java.util.Vector' THEN 1
    WHEN 'java.util.WeakHashMap' THEN 1
    -- java.util.concurrent concrete collection and map classes
    WHEN 'java.util.concurrent.ArrayBlockingQueue' THEN 1
    WHEN 'java.util.concurrent.ConcurrentHashMap' THEN 1
    WHEN 'java.util.concurrent.ConcurrentLinkedDeque' THEN 1
    WHEN 'java.util.concurrent.ConcurrentLinkedQueue' THEN 1
    WHEN 'java.util.concurrent.ConcurrentSkipListMap' THEN 1
    WHEN 'java.util.concurrent.ConcurrentSkipListSet' THEN 1
    WHEN 'java.util.concurrent.CopyOnWriteArrayList' THEN 1
    WHEN 'java.util.concurrent.CopyOnWriteArraySet' THEN 1
    WHEN 'java.util.concurrent.DelayQueue' THEN 1
    WHEN 'java.util.concurrent.LinkedBlockingDeque' THEN 1
    WHEN 'java.util.concurrent.LinkedBlockingQueue' THEN 1
    WHEN 'java.util.concurrent.LinkedTransferQueue' THEN 1
    WHEN 'java.util.concurrent.PriorityBlockingQueue' THEN 1
    WHEN 'java.util.concurrent.SynchronousQueue' THEN 1
    ELSE 0
  END
  = 1;

-- Dominator tree nodes with no ancestor of the same class. Summing the
-- dominated stats of these nodes per class counts every dominated object once.
CREATE PERFETTO TABLE _heap_graph_class_dominated_roots AS
SELECT id, group_key AS type_id
FROM tree_structural_partition_by_group!(
  (
    SELECT
    tree.id,
    coalesce(tree.idom_id, _partition_tree_super_root_fn()) AS parent_id,
    obj.type_id AS group_key
    FROM heap_graph_dominator_tree AS tree
    JOIN heap_graph_object AS obj
    USING (id)
    UNION ALL
    -- provide a single root required by tree partition if heap graph exists.
    SELECT
    _partition_tree_super_root_fn() AS id,
    NULL AS parent_id,
    (SELECT id + 1 FROM heap_graph_class ORDER BY id DESC LIMIT 1) AS group_key
    WHERE
    _partition_tree_super_root_fn() IS NOT NULL
  )
)
WHERE
  parent_id IS NULL
  AND id != _partition_tree_super_root_fn();

-- Heap graph classes are per heap dump, so type_id alone identifies the
-- (upid, graph_sample_ts) of every per-class aggregate below.
CREATE PERFETTO TABLE _heap_graph_class_dominated_stats AS
SELECT
  r.type_id,
  sum(d.dominated_obj_count) AS dominated_obj_count,
  sum(d.dominated_size_bytes) AS dominated_size_bytes,
  sum(d.dominated_native_size_bytes) AS dominated_native_size_bytes
FROM _heap_graph_class_dominated_roots AS r
CROSS JOIN heap_graph_dominator_tree AS d USING (id)
GROUP BY
  r.type_id
ORDER BY
  r.type_id;

CREATE PERFETTO TABLE _heap_graph_class_obj_stats AS
FROM heap_graph_object
|> AGGREGATE
  COUNT(*) AS obj_count,
  SUM(self_size) AS size_bytes,
  SUM(native_size) AS native_size_bytes
GROUP BY
  upid,
  graph_sample_ts,
  type_id
|> ORDER BY type_id;

CREATE PERFETTO TABLE _heap_graph_class_reachable_obj_stats AS
FROM heap_graph_object
|> WHERE reachable = 1
|> AGGREGATE
  COUNT(*) AS obj_count,
  SUM(self_size) AS size_bytes,
  SUM(native_size) AS native_size_bytes
GROUP BY
  type_id
|> ORDER BY type_id;

-- Class-level breakdown of the java heap.
-- Per type name aggregates the object stats and the dominator tree stats.
CREATE PERFETTO TABLE android_heap_graph_class_aggregation(
  -- Process upid
  upid JOINID(process.id),
  -- Heap dump timestamp
  graph_sample_ts TIMESTAMP,
  -- Class type id
  type_id LONG,
  -- Class name (deobfuscated if available)
  type_name STRING,
  -- Deprecated: use is_language_runtime, is_container, and is_static_class instead.
  is_libcore_or_array BOOL,
  -- Whether the class is part of standard Java/Kotlin runtime libraries.
  is_language_runtime BOOL,
  -- Whether the class is an array or standard collection/container.
  is_container BOOL,
  -- Whether the object is a java.lang.Class<*> singleton.
  is_static_class BOOL,
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
)
AS
WITH
  base AS MATERIALIZED (
    SELECT
      a.upid,
      a.graph_sample_ts,
      a.type_id,
      coalesce(cls.deobfuscated_name, cls.name) AS type_name,
      _normalize_java_class_name(coalesce(cls.deobfuscated_name, cls.name)) AS normalized_name,
      a.obj_count,
      a.size_bytes,
      a.native_size_bytes,
      coalesce(r.obj_count, 0) AS reachable_obj_count,
      coalesce(r.size_bytes, 0) AS reachable_size_bytes,
      coalesce(r.native_size_bytes, 0) AS reachable_native_size_bytes,
      coalesce(d.dominated_obj_count, 0) AS dominated_obj_count,
      coalesce(d.dominated_size_bytes, 0) AS dominated_size_bytes,
      coalesce(d.dominated_native_size_bytes, 0) AS dominated_native_size_bytes
    FROM _heap_graph_class_obj_stats AS a
    JOIN heap_graph_class AS cls
      ON a.type_id = cls.id
    LEFT JOIN _heap_graph_class_reachable_obj_stats AS r USING (type_id)
    LEFT JOIN _heap_graph_class_dominated_stats AS d USING (type_id)
  ),
  classified AS MATERIALIZED (
    SELECT
      *,
      _is_language_runtime_normalized(normalized_name) AS is_language_runtime,
      _is_container_normalized(normalized_name) AS is_container
    FROM base
  )
SELECT
  -- core dims
  upid,
  graph_sample_ts,
  type_id,
  type_name,
  -- properties of the type_name
  is_language_runtime
  OR is_container AS is_libcore_or_array,
  is_language_runtime,
  is_container,
  _is_static_class(type_name) AS is_static_class,
  -- metrics
  obj_count,
  size_bytes,
  native_size_bytes,
  reachable_obj_count,
  reachable_size_bytes,
  reachable_native_size_bytes,
  dominated_obj_count,
  dominated_size_bytes,
  dominated_native_size_bytes
FROM classified
ORDER BY
  1,
  2,
  3,
  4;
