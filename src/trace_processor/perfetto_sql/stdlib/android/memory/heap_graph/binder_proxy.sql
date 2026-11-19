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

INCLUDE PERFETTO MODULE android.memory.heap_graph.class_classification;

INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;

INCLUDE PERFETTO MODULE android.memory.heap_graph.excluded_refs;

-- Reachable `android.os.BinderProxy` objects.
CREATE PERFETTO TABLE _heap_graph_binder_proxy_object AS
SELECT id, upid, graph_sample_ts, reference_set_id
FROM heap_graph_object
WHERE
  reachable = 1
  AND type_id IN (
    SELECT id
    FROM _heap_graph_class_classification
    WHERE
      type_name = 'android.os.BinderProxy'
  )
ORDER BY
  id;

-- The `BinderProxy.mDeathRecipients` list of each proxy that has one.
CREATE PERFETTO TABLE _heap_graph_binder_proxy_death_list AS
FROM (
  SELECT p.id AS proxy_id, r.owned_id
  FROM _heap_graph_binder_proxy_object AS p
  CROSS JOIN heap_graph_reference AS r
    ON r.reference_set_id = p.reference_set_id
  WHERE
    p.reference_set_id IS NOT NULL
    AND r.field_name = 'android.os.BinderProxy.mDeathRecipients'
    AND r.owned_id IS NOT NULL
)
|> AGGREGATE MAX(owned_id) AS list_id GROUP BY proxy_id
|> ORDER BY proxy_id;

-- All subclasses of `android.os.RemoteCallbackList` (including itself).
CREATE PERFETTO TABLE _heap_graph_remote_callback_list_class AS
GRAPH DFS TREE
  NODES heap_graph_class
  EDGES (
    FROM heap_graph_class
    |> SELECT superclass_id AS src, id AS dst
  )
  FROM (
    FROM heap_graph_class
    |> SELECT id, coalesce(deobfuscated_name, name, '') AS name
    |> WHERE name = 'android.os.RemoteCallbackList'
  )
|> SELECT id;

-- Wrapper classes skipped when walking up each proxy's dominator chain:
-- standard containers, AIDL `*$Stub$Proxy` classes, and
-- `android.os.RemoteCallbackList` (plus its subclasses, often anonymous, and
-- its nested callback/interface wrappers).
CREATE PERFETTO TABLE _heap_graph_binder_proxy_wrapper_class AS
SELECT id
FROM _heap_graph_class_classification
WHERE
  is_static_class = 0
  AND (is_container = 1
  OR type_name GLOB '*$Stub$Proxy'
  OR type_name GLOB 'android.os.RemoteCallbackList$*')
UNION
SELECT id FROM _heap_graph_remote_callback_list_class ORDER BY id;

-- Standard container classes walked inside `BinderProxy.mDeathRecipients`.
CREATE PERFETTO TABLE _heap_graph_binder_proxy_container_class AS
FROM _heap_graph_class_classification
|> WHERE is_container = 1 AND is_static_class = 0
|> SELECT id
|> ORDER BY id;

-- Dominator-tree ancestors of every reachable `android.os.BinderProxy`
-- (including the proxies themselves), walking upward from `id` to `idom_id`.
CREATE PERFETTO TABLE _heap_graph_binder_proxy_ancestor AS
GRAPH DFS TREE
  NODES heap_graph_dominator_tree
  EDGES (
    FROM heap_graph_dominator_tree
    |> SELECT id AS src, idom_id AS dst
  )
  FROM _heap_graph_binder_proxy_object
|> SELECT id, idom_id
|> ORDER BY id;

-- Outermost wrapper (`wrapper_top_id`) and nearest non-wrapper dominator
-- (`holder_id`) for each proxy's dominator chain. Cutting the ancestor chain
-- at non-wrappers and accumulating down propagates each chain's topmost
-- wrapper and its immediate dominator (the holder).
CREATE PERFETTO TABLE _heap_graph_binder_proxy_holder AS
FROM (
  SELECT a.id, a.idom_id, wc.id AS wrapper_type_id
  FROM _heap_graph_binder_proxy_ancestor AS a
  LEFT JOIN heap_graph_object AS po ON po.id = a.idom_id
  LEFT JOIN _heap_graph_binder_proxy_wrapper_class AS wc ON wc.id = po.type_id
)
|> EXTEND
  iif(wrapper_type_id IS NOT NULL, idom_id, NULL) AS parent_id,
  iif(wrapper_type_id IS NOT NULL, NULL, id) AS top_seed,
  iif(wrapper_type_id IS NOT NULL, NULL, idom_id) AS holder_seed
|> TREE ACCUMULATE DOWN
  MAX(top_seed) AS wrapper_top_id,
  MAX(holder_seed) AS holder_id
|> SELECT id, wrapper_top_id, holder_id
|> ORDER BY id;

-- Container objects reachable from `android.os.BinderProxy.mDeathRecipients`,
-- tagged with the root `mDeathRecipients` list `list_id` they belong to.
-- Walking through containers handles both plain `ArrayList` and
-- `Collections$SynchronizedRandomAccessList` -> `ArrayList` -> `Object[]`.
CREATE PERFETTO TABLE _heap_graph_binder_proxy_death_container AS
GRAPH DFS TREE
  NODES (
    SELECT o.id
    FROM (
      SELECT
        EXISTS (SELECT 1 FROM _heap_graph_binder_proxy_death_list) AS has_dl
    ) AS g
    CROSS JOIN heap_graph_object AS o
    WHERE
      g.has_dl = 1
      AND o.reachable = 1
      AND o.type_id IN _heap_graph_binder_proxy_container_class
  )
  EDGES _heap_graph_strong_edges
  FROM (
    FROM _heap_graph_binder_proxy_death_list
    |> SELECT list_id AS id
    |> DISTINCT
  )
|> EXTEND iif(parent_id IS NULL, id, NULL) AS list_seed
|> TREE ACCUMULATE DOWN MAX(list_seed) AS list_id
|> SELECT id AS container_id, list_id
|> ORDER BY list_id;

-- Reachable `android.os.BinderProxy` objects in ART heap dumps, annotated with
-- their wrapping AIDL `*$Stub$Proxy` class, their nearest non-wrapper
-- dominator (`holder_*`), and any `IBinder.DeathRecipient` registrations in
-- `BinderProxy.mDeathRecipients`.
--
-- A holder does not mean a leak: most proxies belong to live sessions,
-- registered callbacks, or process-lifetime service caches. Across heap dumps
-- of peer processes, `replace(proxy_class_name, '$Stub$Proxy', '$Stub')`
-- matches `stub_class_name` in `android_heap_graph_binder_stub`, though a
-- single-process dump cannot tell which remote process owns a stub.
CREATE PERFETTO TABLE android_heap_graph_binder_proxy(
  -- Heap graph object id of the `android.os.BinderProxy` instance.
  id JOINID(heap_graph_object.id),
  -- Process upid that owns the heap dump.
  upid JOINID(process.id),
  -- Heap dump timestamp.
  graph_sample_ts TIMESTAMP,
  -- Class id of the `*$Stub$Proxy` object directly referencing this proxy, or
  -- NULL if none (e.g. raw `IBinder` tokens).
  proxy_class_id JOINID(heap_graph_class.id),
  -- Java class name (deobfuscated if available) of the `*$Stub$Proxy` object
  -- directly referencing this proxy, or NULL if none. When multiple
  -- `*$Stub$Proxy` classes reference the same proxy, the first by name wins.
  proxy_class_name STRING,
  -- Heap graph object id of the nearest dominator that is not a container,
  -- `*$Stub$Proxy`, or `RemoteCallbackList` wrapper, or NULL if no single
  -- non-wrapper object dominates the proxy.
  holder_id JOINID(heap_graph_object.id),
  -- Class id of `holder_id`, or NULL if `holder_id` is NULL.
  holder_type_id JOINID(heap_graph_class.id),
  -- Java class name (deobfuscated if available) of `holder_id` (e.g.
  -- `java.lang.Class<Foo>` when held by a static field), or NULL if
  -- `holder_id` is NULL.
  holder_class_name STRING,
  -- Field on `holder_id` (deobfuscated if available) that directly references
  -- the proxy or its outermost wrapper, or NULL if `holder_id` is NULL or
  -- dominates through multiple intermediate non-wrapper paths.
  holder_field_name STRING,
  -- Number of recipient entries in `BinderProxy.mDeathRecipients` (0 when the
  -- list is empty or the field is absent from the dump).
  death_recipient_count LONG,
  -- First recipient Java class name (deobfuscated if available) ordered by
  -- name in `BinderProxy.mDeathRecipients`, or NULL when
  -- `death_recipient_count` is 0.
  death_recipient_class_name STRING
)
AS
WITH
  proxy_classes AS (
    SELECT
      e.dst AS proxy_id,
      c.id AS proxy_class_id,
      c.type_name AS proxy_class_name,
      ROW_NUMBER() OVER (PARTITION BY e.dst ORDER BY c.type_name) AS rn
    FROM _heap_graph_strong_edges AS e
    JOIN heap_graph_object AS o ON o.id = e.src
    JOIN _heap_graph_class_classification AS c ON c.id = o.type_id
    WHERE
      e.dst IN (SELECT id FROM _heap_graph_binder_proxy_object)
      AND c.is_static_class = 0
      AND c.type_name GLOB '*$Stub$Proxy'
  ),
  death_by_list AS (
    SELECT
      dc.list_id,
      COUNT(*) AS death_recipient_count,
      MIN(rc.type_name) AS death_recipient_class_name
    FROM _heap_graph_binder_proxy_death_container AS dc
    JOIN heap_graph_object AS co ON co.id = dc.container_id
    CROSS JOIN heap_graph_reference AS r
      ON r.reference_set_id = co.reference_set_id
    JOIN heap_graph_object AS ro ON ro.id = r.owned_id
    JOIN _heap_graph_class_classification AS rc ON rc.id = ro.type_id
    WHERE
      co.reference_set_id IS NOT NULL
      AND r.owned_id IS NOT NULL
      AND r.field_name != 'java.lang.Object.shadow$_klass_'
      AND rc.is_container = 0
      AND rc.is_static_class = 0
    GROUP BY
      dc.list_id
  )
SELECT
  p.id,
  p.upid,
  p.graph_sample_ts,
  pc.proxy_class_id,
  pc.proxy_class_name,
  h.holder_id,
  ho.type_id AS holder_type_id,
  hc.type_name AS holder_class_name,
  (
    SELECT MIN(coalesce(r.deobfuscated_field_name, r.field_name))
    FROM heap_graph_reference AS r
    WHERE
      ho.reference_set_id IS NOT NULL
      AND r.reference_set_id = ho.reference_set_id
      AND r.owned_id = h.wrapper_top_id
      AND NOT (ho.type_id IN _ref_type_ids
      AND r.field_name = 'java.lang.ref.Reference.referent')
  ) AS holder_field_name,
  coalesce(dr.death_recipient_count, 0) AS death_recipient_count,
  dr.death_recipient_class_name
FROM _heap_graph_binder_proxy_object AS p
LEFT JOIN proxy_classes AS pc ON pc.proxy_id = p.id AND pc.rn = 1
LEFT JOIN _heap_graph_binder_proxy_holder AS h ON h.id = p.id
LEFT JOIN heap_graph_object AS ho ON ho.id = h.holder_id
LEFT JOIN _heap_graph_class_classification AS hc ON hc.id = ho.type_id
LEFT JOIN _heap_graph_binder_proxy_death_list AS pdl ON pdl.proxy_id = p.id
LEFT JOIN death_by_list AS dr ON dr.list_id = pdl.list_id
ORDER BY
  p.id;
