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

INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;

INCLUDE PERFETTO MODULE android.memory.heap_graph.excluded_refs;

INCLUDE PERFETTO MODULE graphs.scan;

-- All subclasses of `android.os.Binder` (including `android.os.Binder` itself).
CREATE PERFETTO TABLE _binder_classes AS
GRAPH DFS TREE
  NODES (SELECT id FROM heap_graph_class)
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
|> SELECT id AS class_id;

-- Upward superclass chain from each Binder subclass to its ancestors.
CREATE PERFETTO TABLE _binder_class_ancestry AS
SELECT id AS ancestor_id, class_id, depth
FROM _graph_scan!(
  (
    SELECT id AS source_node_id, superclass_id AS dest_node_id
    FROM heap_graph_class
    WHERE superclass_id IS NOT NULL
  ),
  (SELECT class_id AS id, class_id, 0 AS depth FROM _binder_classes),
  (class_id, depth),
  (SELECT id, class_id, depth + 1 AS depth FROM $table)
);

-- Candidate ancestor names for resolving the AIDL interface `$Stub` class.
CREATE PERFETTO TABLE _stub_ancestors AS
SELECT
  ca.class_id,
  coalesce(anc.deobfuscated_name, anc.name, '') AS anc_name,
  ca.depth
FROM _binder_class_ancestry AS ca
JOIN heap_graph_class AS anc ON ca.ancestor_id = anc.id
WHERE
  ca.depth > 0
  AND coalesce(anc.deobfuscated_name, anc.name, '') NOT IN (
    'android.os.Binder',
    'com.google.android.aidl.BaseStub',
    'java.lang.Object',
    ''
  );

-- Resolved concrete class name and AIDL `$Stub` interface name per Binder
-- `heap_graph_class.id`.
CREATE PERFETTO TABLE _resolved_stub_class AS
SELECT
  bs.class_id,
  coalesce(cls.deobfuscated_name, cls.name, '') AS class_name,
  coalesce(
    (
      SELECT sa.anc_name
      FROM _stub_ancestors AS sa
      WHERE
        sa.class_id = bs.class_id
        AND sa.anc_name GLOB '*$Stub'
      ORDER BY
        sa.depth ASC
      LIMIT 1
    ),
    (
      SELECT sa.anc_name
      FROM _stub_ancestors AS sa
      WHERE
        sa.class_id = bs.class_id
      ORDER BY
        sa.depth ASC
      LIMIT 1
    ),
    coalesce(cls.deobfuscated_name, cls.name, '')
  ) AS interface_name
FROM _binder_classes AS bs
JOIN heap_graph_class AS cls ON bs.class_id = cls.id;

-- Strong reference edges between reachable heap objects (excluding weak,
-- phantom and finalizer referent edges).
CREATE PERFETTO TABLE _heap_edges AS
SELECT ref.owner_id AS src, ref.owned_id AS dst
FROM heap_graph_object AS o
CROSS JOIN heap_graph_reference AS ref USING (reference_set_id)
WHERE
  o.reachable
  AND o.reference_set_id IS NOT NULL
  AND ref.owned_id IS NOT NULL
  AND NOT (o.type_id IN _ref_type_ids
  AND ref.field_name = 'java.lang.ref.Reference.referent');

-- Binder stub instances held as `ROOT_JNI_GLOBAL` (referenced across Binder
-- IPC from another process via the kernel/libbinder JNI global reference).
CREATE PERFETTO TABLE _binder_roots AS
SELECT id
FROM heap_graph_object
WHERE
  root_type = 'ROOT_JNI_GLOBAL'
  AND type_id IN (SELECT class_id FROM _binder_classes);

-- All other GC roots of the process (its own roots).
CREATE PERFETTO TABLE _intrinsic_roots AS
SELECT id
FROM heap_graph_object
WHERE
  root_type IS NOT NULL
  AND id NOT IN (SELECT id FROM _binder_roots);

CREATE PERFETTO TABLE _reach_intrinsic AS
GRAPH DFS TREE
  NODES (SELECT id FROM heap_graph_object)
  EDGES _heap_edges
  FROM _intrinsic_roots
|> SELECT id;

CREATE PERFETTO TABLE _reach_binder AS
GRAPH DFS TREE
  NODES (SELECT id FROM heap_graph_object)
  EDGES _heap_edges
  FROM _binder_roots
|> SELECT id;

-- Heap objects reachable from `ROOT_JNI_GLOBAL` Binder stubs, but NOT
-- reachable from any of the process's own GC roots.
CREATE PERFETTO TABLE _remote_only_nodes AS
SELECT id, self_size + coalesce(native_size, 0) AS total_size
FROM heap_graph_object
WHERE
  id IN (SELECT id FROM _reach_binder)
  AND id NOT IN (SELECT id FROM _reach_intrinsic);

-- DFS spanning forest over the remote-only subgraph rooted at untracked Binder
-- stubs, accumulating subtree object counts and bytes up to each root stub.
-- When multiple untracked stubs share reachable objects, each object is
-- assigned to the first stub whose traversal reaches it.
CREATE PERFETTO TABLE _ro_tree AS
GRAPH DFS TREE
  NODES _remote_only_nodes
  EDGES (
    SELECT src, dst
    FROM _heap_edges
    WHERE
      src IN (SELECT id FROM _remote_only_nodes)
      AND dst IN (SELECT id FROM _remote_only_nodes)
  )
  FROM (
    SELECT id FROM _binder_roots WHERE id IN (SELECT id FROM _remote_only_nodes)
  )
|> TREE ACCUMULATE UP
  COUNT(*) AS remote_only_obj_count,
  SUM(total_size) AS remote_only_size_bytes
|> SELECT id, parent_id, remote_only_obj_count, remote_only_size_bytes;

CREATE PERFETTO TABLE _ro_by_stub AS
SELECT id, remote_only_obj_count, remote_only_size_bytes
FROM _ro_tree
WHERE
  parent_id IS NULL;

-- Reachable `android.os.Binder` stub instances in ART heap dumps, annotated
-- with their concrete class, resolved AIDL `$Stub` interface, whether they are
-- held across Binder IPC (`ROOT_JNI_GLOBAL`), and whether the process's own GC
-- roots no longer reach them (`is_untracked`).
CREATE PERFETTO TABLE android_heap_graph_binder_stub(
  -- Heap graph object id of the Binder stub instance.
  id JOINID(heap_graph_object.id),
  -- Process upid that owns the heap dump.
  upid JOINID(process.id),
  -- Heap dump timestamp.
  graph_sample_ts TIMESTAMP,
  -- Concrete Java class name of the Binder stub (deobfuscated if available).
  class_name STRING,
  -- Nearest AIDL `*$Stub` superclass name, or nearest non-`Binder` superclass
  -- if the stub extends `android.os.Binder` directly.
  interface_name STRING,
  -- Whether this stub is held as a `ROOT_JNI_GLOBAL` GC root (referenced by
  -- another process via Binder).
  is_remote_held BOOL,
  -- Whether this stub is held as `ROOT_JNI_GLOBAL` and is NOT reachable from
  -- any of the process's own GC roots (kept alive only by other processes).
  is_untracked BOOL,
  -- Count of remote-only objects (reachable only from untracked Binder stubs,
  -- self inclusive) attributed to this stub instance by a DFS spanning forest.
  remote_only_obj_count LONG,
  -- Bytes (`self_size + native_size`) of remote-only objects attributed to
  -- this stub instance. Sum across a dump is exact; split across stubs sharing
  -- subgraph nodes depends on traversal order.
  remote_only_size_bytes LONG,
  -- Total dominated bytes (`self_size + native_size`) of this stub instance in
  -- the heap dominator tree.
  dominated_size_bytes LONG
)
AS
SELECT
  o.id,
  o.upid,
  o.graph_sample_ts,
  r.class_name,
  r.interface_name,
  iif(o.root_type = 'ROOT_JNI_GLOBAL', 1, 0) AS is_remote_held,
  iif(ro.id IS NOT NULL, 1, 0) AS is_untracked,
  coalesce(ro_stats.remote_only_obj_count, 0) AS remote_only_obj_count,
  coalesce(ro_stats.remote_only_size_bytes, 0) AS remote_only_size_bytes,
  coalesce(
    dt.dominated_size_bytes + coalesce(dt.dominated_native_size_bytes, 0),
    0
  ) AS dominated_size_bytes
FROM heap_graph_object AS o
JOIN _resolved_stub_class AS r ON o.type_id = r.class_id
LEFT JOIN _remote_only_nodes AS ro ON o.id = ro.id
LEFT JOIN _ro_by_stub AS ro_stats ON o.id = ro_stats.id
LEFT JOIN heap_graph_dominator_tree AS dt ON o.id = dt.id
WHERE
  o.reachable
ORDER BY
  o.id;

-- Per-class summary of `ROOT_JNI_GLOBAL` `android.os.Binder` stubs in an ART
-- heap dump, showing how many instances are no longer referenced by the
-- process's own GC roots (`untracked_count`) and how much memory is kept alive
-- only through them (`remote_only_size_bytes`).
--
-- Not all untracked stubs are leaks: some are legitimately held by clients or
-- by `servicemanager` (`is_single_instance` flags single-instance classes).
-- Totals per dump are exact; when stub classes share reachable objects, the
-- per-class `remote_only_size_bytes` split is approximate (assigned in DFS
-- traversal order).
CREATE PERFETTO TABLE android_heap_graph_binder_stub_summary(
  -- Process upid that owns the heap dump.
  upid JOINID(process.id),
  -- Heap dump timestamp.
  graph_sample_ts TIMESTAMP,
  -- Concrete Java class name of the Binder stub (deobfuscated if available).
  class_name STRING,
  -- Nearest AIDL `*$Stub` superclass name, or nearest non-`Binder` superclass
  -- if the stub extends `android.os.Binder` directly.
  interface_name STRING,
  -- Total `ROOT_JNI_GLOBAL` instances of this stub class in the dump.
  instance_count LONG,
  -- Instances held as `ROOT_JNI_GLOBAL` that are NOT reachable from the
  -- process's own GC roots (kept alive only by other processes).
  untracked_count LONG,
  -- Count of remote-only objects attributed to untracked instances of this
  -- stub class.
  remote_only_obj_count LONG,
  -- Bytes (`self_size + native_size`) of remote-only objects attributed to
  -- untracked instances of this stub class. Exact in dump total; approximate
  -- per class when classes share reachable objects.
  remote_only_size_bytes LONG,
  -- Sum of dominator-tree dominated bytes (`self_size + native_size`) across
  -- `ROOT_JNI_GLOBAL` instances of this stub class.
  dominated_size_bytes LONG,
  -- Maximum single-instance dominated bytes among `ROOT_JNI_GLOBAL` instances
  -- of this stub class.
  max_instance_dominated_bytes LONG,
  -- Example untracked `heap_graph_object.id` of this class (the one with the
  -- largest attributed `remote_only_size_bytes`), or NULL if `untracked_count`
  -- is 0. Useful as a starting point in the Heap Dump Explorer UI.
  example_id JOINID(heap_graph_object.id),
  -- Whether `instance_count = 1`. In `system_server`, a single untracked
  -- instance is often a registered system service held only by
  -- `servicemanager` rather than a leaked per-client session or callback.
  is_single_instance BOOL
)
AS
WITH
  ranked AS (
    SELECT
      *,
      row_number() OVER (
        PARTITION BY
          upid,
          graph_sample_ts,
          class_name,
          interface_name
        ORDER BY
          is_untracked DESC,
          remote_only_size_bytes DESC,
          dominated_size_bytes DESC,
          id ASC
      ) AS rn
    FROM android_heap_graph_binder_stub
    WHERE
      is_remote_held
  )
SELECT
  upid,
  graph_sample_ts,
  class_name,
  interface_name,
  count(*) AS instance_count,
  sum(is_untracked) AS untracked_count,
  sum(remote_only_obj_count) AS remote_only_obj_count,
  sum(remote_only_size_bytes) AS remote_only_size_bytes,
  sum(dominated_size_bytes) AS dominated_size_bytes,
  max(dominated_size_bytes) AS max_instance_dominated_bytes,
  max(iif(rn = 1 AND is_untracked = 1, id, NULL)) AS example_id,
  iif(count(*) = 1, 1, 0) AS is_single_instance
FROM ranked
GROUP BY
  upid,
  graph_sample_ts,
  class_name,
  interface_name
ORDER BY
  upid ASC,
  graph_sample_ts ASC,
  remote_only_size_bytes DESC,
  dominated_size_bytes DESC,
  untracked_count DESC,
  instance_count DESC,
  class_name ASC;
