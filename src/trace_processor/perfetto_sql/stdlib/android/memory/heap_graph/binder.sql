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

-- All subclasses of `android.os.Binder` (including `android.os.Binder` itself),
-- with the nearest `*$Stub` class at or above each, NULL when there is none. The
-- class tree is cut at the stub classes, so accumulating down from each cut picks
-- the closest stub.
CREATE PERFETTO TABLE _heap_graph_binder_class AS
GRAPH DFS TREE
  NODES heap_graph_class
  EDGES (
    FROM heap_graph_class
    |> SELECT superclass_id AS src, id AS dst
  )
  FROM (
    FROM heap_graph_class
    |> SELECT id, coalesce(deobfuscated_name, name, '') AS name
    |> WHERE name = 'android.os.Binder'
  )
|> SELECT
  id,
  parent_id AS superclass_id,
  coalesce(deobfuscated_name, name, '') AS name
|> EXTEND name GLOB '*$Stub' AS is_stub
|> EXTEND
  iif(is_stub, NULL, superclass_id) AS parent_id,
  iif(is_stub, id, NULL) AS stub_id
|> TREE ACCUMULATE DOWN MAX(stub_id) AS nearest_stub_id
|> SELECT id, superclass_id, name, nearest_stub_id;

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

-- Reachable objects, flagged with whether they are one of the process's own GC
-- roots: every root except the `ROOT_JNI_GLOBAL` Binder stubs.
CREATE PERFETTO TABLE _heap_graph_binder_candidate AS
SELECT
  id,
  self_size,
  native_size,
  iif(
    root_type IS NOT NULL
    AND id NOT IN (SELECT id FROM _heap_graph_binder_roots),
    0,
    1
  ) AS not_own_root
FROM heap_graph_object
WHERE
  reachable = 1;

-- The stubs, by (deobfuscated) class name, which keeps the split between
-- classes comparable across heap dumps. Stubs of one class have nothing stable
-- to tell them apart, so they are in no particular order.
CREATE PERFETTO TABLE _heap_graph_binder_stub_seed AS
SELECT r.id
FROM _heap_graph_binder_roots AS r
JOIN heap_graph_object AS o ON o.id = r.id
JOIN _heap_graph_binder_class AS c ON c.id = o.type_id
ORDER BY
  c.name;

-- The process's own roots, then the stubs: the order the DFS below searches.
CREATE PERFETTO TABLE _heap_graph_binder_seed AS
SELECT id FROM _heap_graph_binder_candidate WHERE not_own_root = 0
UNION ALL
SELECT id FROM _heap_graph_binder_stub_seed;

-- Remote-only objects (reachable from the `ROOT_JNI_GLOBAL` Binder stubs but
-- not from the process's own roots) as a BFS forest from the stubs. The inner
-- DFS searches own roots first, so an object has no own root above it exactly
-- when own roots cannot reach it. Stubs own roots reach are not nodes, so they
-- drop out, and the forest roots (parent_id IS NULL) are exactly the
-- remote-only stubs. Each object belongs to the closest stub; ties go to the
-- first stub in seed order. The search keeps the stubs' classes in seed order
-- at every depth, so the total per class depends on neither the order of the
-- edges nor the order of the stubs within a class.
CREATE PERFETTO TABLE _heap_graph_binder_remote_only_by_stub AS
GRAPH BFS TREE
  NODES (
    GRAPH DFS TREE
      NODES _heap_graph_binder_candidate
      EDGES _heap_graph_strong_edges
      FROM _heap_graph_binder_seed
    |> TREE ACCUMULATE DOWN MIN(not_own_root) AS remote_only
    |> WHERE remote_only = 1
    |> SELECT id, self_size, native_size
  )
  EDGES _heap_graph_strong_edges
  FROM _heap_graph_binder_stub_seed
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
  -- stub wins, then first class name). The split between stubs of the same
  -- class is arbitrary: compare heap dumps by class, not by instance.
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
  c.name AS class_name,
  superclass.nearest_stub_id AS stub_class_id,
  stub.name AS stub_class_name,
  ro_stats.id IS NOT NULL AS is_remote_only,
  coalesce(ro_stats.remote_only_obj_count, 0) AS remote_only_obj_count,
  coalesce(ro_stats.remote_only_size_bytes, 0) AS remote_only_size_bytes,
  coalesce(ro_stats.remote_only_native_size_bytes, 0) AS remote_only_native_size_bytes
FROM _heap_graph_binder_roots AS b
JOIN heap_graph_object AS o USING (id)
JOIN _heap_graph_binder_class AS c ON c.id = o.type_id
-- The lookup starts at the superclass, so an implementation that is itself named
-- `*$Stub` (e.g. `FooService$Stub extends IFoo$Stub`) resolves to the AIDL
-- `IFoo$Stub`.
LEFT JOIN _heap_graph_binder_class AS superclass
  ON superclass.id = c.superclass_id
LEFT JOIN _heap_graph_binder_class AS stub
  ON stub.id = superclass.nearest_stub_id
LEFT JOIN _heap_graph_binder_remote_only_by_stub AS ro_stats
  ON o.id = ro_stats.id
ORDER BY
  o.id;
