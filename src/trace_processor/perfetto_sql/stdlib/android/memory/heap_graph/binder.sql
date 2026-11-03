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
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

INCLUDE PERFETTO MODULE android.memory.heap_graph.excluded_refs;

-- All subclasses of `android.os.Binder` (including `android.os.Binder` itself)
-- as a tree in which `parent_id` is the superclass.
CREATE PERFETTO TABLE _heap_graph_binder_class AS
GRAPH DFS TREE
  NODES (
    SELECT
      id,
      coalesce(deobfuscated_name, name, '') AS name,
      coalesce(deobfuscated_name, name, '') GLOB '*$Stub' AS is_stub
    FROM heap_graph_class
  )
  EDGES (
    SELECT superclass_id AS src, id AS dst
    FROM heap_graph_class
    WHERE
      superclass_id IS NOT NULL
  )
  FROM (
    SELECT id
    FROM heap_graph_class
    WHERE
      coalesce(deobfuscated_name, name, '') = 'android.os.Binder'
  )
|> SELECT id, parent_id, name, is_stub;

-- The nearest `*$Stub` class at or above every Binder class: the class tree
-- is cut at the stub classes, so accumulating down from each cut picks the
-- closest stub. NULL when there is no stub.
CREATE PERFETTO TABLE _heap_graph_binder_class_stub AS
FROM (
  SELECT
    id,
    iif(is_stub, NULL, parent_id) AS parent_id,
    iif(is_stub, id, NULL) AS stub_id
  FROM _heap_graph_binder_class
)
|> TREE ACCUMULATE DOWN MAX(stub_id) AS stub_class_id
|> SELECT id, stub_class_id;

-- Concrete class name and nearest `*$Stub` superclass per Binder class. The
-- lookup starts at the superclass, so an implementation that is itself named
-- `*$Stub` (e.g. `FooService$Stub extends IFoo$Stub`) resolves to the AIDL
-- `IFoo$Stub`. NULL when no superclass is a stub (e.g. direct
-- `android.os.Binder` subclasses).
CREATE PERFETTO TABLE _heap_graph_binder_resolved_stub_class AS
SELECT
  c.id AS class_id,
  c.name AS class_name,
  s.stub_class_id,
  stub.name AS stub_class_name
FROM _heap_graph_binder_class AS c
LEFT JOIN _heap_graph_binder_class_stub AS s ON s.id = c.parent_id
LEFT JOIN _heap_graph_binder_class AS stub ON stub.id = s.stub_class_id;

-- Materialized copy of `_heap_graph_strong_edges`: both walks below read it.
CREATE PERFETTO TABLE _heap_graph_binder_edges AS
SELECT src, dst FROM _heap_graph_strong_edges;

-- `android.os.Binder` objects held by a JNI global reference
-- (`ROOT_JNI_GLOBAL`).
CREATE PERFETTO TABLE _heap_graph_binder_roots AS
SELECT id
FROM heap_graph_object
WHERE
  root_type = 'ROOT_JNI_GLOBAL'
  AND type_id IN (SELECT id FROM _heap_graph_binder_class)
ORDER BY
  id;

-- Objects reachable from the process's own GC roots: every root except the
-- `ROOT_JNI_GLOBAL` Binder stubs.
CREATE PERFETTO TABLE _heap_graph_binder_reach_intrinsic AS
GRAPH DFS TREE
  NODES heap_graph_object
  EDGES _heap_graph_binder_edges
  FROM (
    SELECT id
    FROM heap_graph_object
    WHERE
      root_type IS NOT NULL
      AND id NOT IN (SELECT id FROM _heap_graph_binder_roots)
  )
|> SELECT id;

-- Remote-only objects (reachable from the `ROOT_JNI_GLOBAL` Binder stubs but
-- not from the process's own roots) as a BFS forest from the stubs. Stubs
-- reachable from the process's own roots are not nodes, so they drop out, and
-- the forest roots (parent_id IS NULL) are exactly the remote-only stubs. Each
-- object belongs to the closest stub; ties go to the stub first in order of
-- (deobfuscated) class name, which keeps the split comparable across heap
-- dumps, then id. As in `_heap_graph_object_min_depth_tree`, each object's
-- references are taken in order of the object referred to, so the split is
-- stable.
CREATE PERFETTO TABLE _heap_graph_binder_remote_only_by_stub AS
GRAPH BFS TREE
  NODES (
    SELECT id, self_size, native_size
    FROM heap_graph_object
    WHERE
      id NOT IN (SELECT id FROM _heap_graph_binder_reach_intrinsic)
  )
  EDGES (SELECT src, dst FROM _heap_graph_binder_edges ORDER BY dst)
  FROM (
    SELECT r.id
    FROM _heap_graph_binder_roots AS r
    JOIN heap_graph_object AS o ON o.id = r.id
    JOIN _heap_graph_binder_class AS c ON c.id = o.type_id
    ORDER BY
      c.name,
      r.id
  )
|> TREE ACCUMULATE UP
  COUNT(*) AS remote_only_obj_count,
  SUM(self_size) AS remote_only_size_bytes,
  SUM(native_size) AS remote_only_native_size_bytes
|> WHERE parent_id IS NULL
|> ORDER BY id
|> SELECT
  id,
  remote_only_obj_count,
  remote_only_size_bytes,
  remote_only_native_size_bytes;

-- `android.os.Binder` objects held by a JNI global reference
-- (`ROOT_JNI_GLOBAL`) in ART heap dumps. For Binder objects this is typically
-- `JavaBBinder` keeping the object alive while other processes hold handles to
-- it. Annotated with the concrete class, the nearest `*$Stub` superclass, and
-- whether the process's own GC roots no longer reach the object
-- (`is_remote_only`). Remote-only does not mean leaked: stubs can be
-- legitimately held by clients, `servicemanager`, in-flight transactions, or
-- peers that have not yet released their handles.
CREATE PERFETTO TABLE android_heap_graph_binder_stub(
  -- Heap graph object id of the Binder object.
  id JOINID(heap_graph_object.id),
  -- Process upid that owns the heap dump.
  upid JOINID(process.id),
  -- Heap dump timestamp.
  graph_sample_ts TIMESTAMP,
  -- Class id of the Binder object.
  type_id JOINID(heap_graph_class.id),
  -- Concrete Java class name of the Binder object (deobfuscated if available).
  class_name STRING,
  -- Class id of the nearest `*$Stub` superclass, or NULL if there is none.
  stub_class_id JOINID(heap_graph_class.id),
  -- Nearest `*$Stub` superclass (e.g. `IFoo$Stub` for an AIDL
  -- implementation), or NULL if there is none (e.g. direct `android.os.Binder`
  -- subclasses).
  stub_class_name STRING,
  -- Whether the object is NOT reachable from any of the process's own GC
  -- roots, i.e. it is kept alive only by its JNI global reference (typically
  -- remote handles).
  is_remote_only BOOL,
  -- Count of remote-only objects (reachable only from remote-only Binder stubs,
  -- self inclusive) attributed to this stub instance by a BFS forest (closest
  -- stub wins).
  remote_only_obj_count LONG,
  -- Java bytes (`self_size`) of remote-only objects attributed to this stub
  -- instance. Sum across a dump is exact; an object reachable from several
  -- stubs is attributed to the closest one.
  remote_only_size_bytes LONG,
  -- Native bytes (`native_size`) of remote-only objects attributed to this
  -- stub instance.
  remote_only_native_size_bytes LONG
)
AS
SELECT
  o.id,
  o.upid,
  o.graph_sample_ts,
  o.type_id,
  r.class_name,
  r.stub_class_id,
  r.stub_class_name,
  ro_stats.id IS NOT NULL AS is_remote_only,
  coalesce(ro_stats.remote_only_obj_count, 0) AS remote_only_obj_count,
  coalesce(ro_stats.remote_only_size_bytes, 0) AS remote_only_size_bytes,
  coalesce(ro_stats.remote_only_native_size_bytes, 0) AS remote_only_native_size_bytes
FROM _heap_graph_binder_roots AS b
JOIN heap_graph_object AS o USING (id)
JOIN _heap_graph_binder_resolved_stub_class AS r ON o.type_id = r.class_id
LEFT JOIN _heap_graph_binder_remote_only_by_stub AS ro_stats
  ON o.id = ro_stats.id
ORDER BY
  o.id;
