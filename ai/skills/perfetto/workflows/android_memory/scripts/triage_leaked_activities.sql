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

-- Leaked Activity check (the LeakCanary heuristic): a process normally has
-- at most one live instance of each Activity class, and roughly one
-- android.view.ViewRootImpl per visible window. Many instances of the same
-- Activity, or more Activities than ViewRootImpls, means destroyed
-- Activities (and their whole view trees) are being retained.
--
-- For every reachable Activity instance, this walks the shortest path from a
-- GC root down to it (the same path LeakCanary reports) and groups instances
-- whose paths have the same shape, so "N of M FooActivity instances are
-- held by X" is visible at a glance.
--
-- Result set 1, one row per dump and Activity class:
--   process_name, graph_sample_ts, activity_class, instance_count,
--   view_root_impl_count (same for all rows of a dump)
--
-- Result set 2, one row per dump, Activity class and distinct path shape:
--   process_name, graph_sample_ts, activity_class, instance_count (with this
--   path shape), example_object_id, gc_root_path
-- gc_root_path reads root to Activity as
--   [ROOT_TYPE] RootClass .field -> NextClass .field -> ... -> Activity
-- Runs of the same class (linked lists, nested contexts) are collapsed to
-- "Class xN"; N is from the example object and varies between instances.

INCLUDE PERFETTO MODULE android.memory.heap_graph.class_relationship;

INCLUDE PERFETTO MODULE android.memory.heap_graph.class_tree;

CREATE OR REPLACE PERFETTO TABLE _leak_activities AS
SELECT
  o.id,
  o.upid,
  o.graph_sample_ts,
  coalesce(c.deobfuscated_name, c.name) AS activity_class
FROM android_heap_graph_class_find_descendants!(
  (SELECT id FROM heap_graph_class WHERE name = 'android.app.Activity')
) AS d
JOIN heap_graph_object AS o
  ON o.type_id = d.id
JOIN heap_graph_class AS c
  ON c.id = d.id
WHERE
  o.reachable;

CREATE OR REPLACE PERFETTO TABLE _leak_view_roots AS
SELECT o.upid, o.graph_sample_ts, count() AS view_root_impl_count
FROM heap_graph_object AS o
JOIN heap_graph_class AS c ON c.id = o.type_id
WHERE
  c.name = 'android.view.ViewRootImpl'
  AND o.reachable
GROUP BY
  o.upid,
  o.graph_sample_ts;

-- Shortest path from a GC root to each Activity (BFS tree from the roots).
CREATE OR REPLACE PERFETTO TABLE _leak_chain AS
WITH RECURSIVE
  chain(start_id, id, prev_id, depth) AS (
    SELECT id, id, NULL, 0 FROM _leak_activities
    UNION ALL
    SELECT chain.start_id, t.parent_id, chain.id, chain.depth + 1
    FROM chain
    JOIN _heap_graph_object_min_depth_tree AS t
      ON t.id = chain.id
    WHERE
      t.parent_id IS NOT NULL
  )
SELECT
  ch.start_id,
  ch.id,
  ch.depth,
  coalesce(c.deobfuscated_name, c.name) AS cls,
  o.root_type
FROM chain AS ch
JOIN heap_graph_object AS o
  ON o.id = ch.id
JOIN heap_graph_class AS c
  ON c.id = o.type_id;

-- Keep only the first object of each run of same-class objects (root side).
CREATE OR REPLACE PERFETTO TABLE _leak_runs AS
SELECT *
FROM (
  SELECT
    start_id,
    id,
    depth,
    cls,
    root_type,
    lag(cls) OVER (PARTITION BY start_id ORDER BY depth DESC) AS prev_cls,
    lag(id) OVER (PARTITION BY start_id ORDER BY depth DESC) AS prev_id
  FROM _leak_chain
)
WHERE
  prev_cls IS NULL
  OR prev_cls != cls;

CREATE OR REPLACE PERFETTO TABLE _leak_hops AS
SELECT
  r.start_id,
  r.depth,
  r.depth
  - ifnull(
    lead(r.depth) OVER (PARTITION BY r.start_id ORDER BY r.depth DESC),
    -1
  ) AS run_len,
  iif(r.root_type IS NOT NULL, '[' || r.root_type || '] ', '')
  || iif(
    f.field_name IS NOT NULL,
    '.'
    || replace(
      f.field_name,
      rtrim(f.field_name, replace(f.field_name, '.', '')),
      ''
    )
    || ' -> ',
    ''
  )
  || r.cls AS label
FROM _leak_runs AS r
LEFT JOIN (
  SELECT
    owner_id,
    owned_id,
    min(coalesce(deobfuscated_field_name, field_name)) AS field_name
  FROM heap_graph_reference
  WHERE
    owned_id IN (SELECT id FROM _leak_runs)
  GROUP BY
    owner_id,
    owned_id
) AS f
  ON f.owner_id = r.prev_id
  AND f.owned_id = r.id;

CREATE OR REPLACE PERFETTO TABLE _leak_paths AS
SELECT
  start_id,
  group_concat(label, ' ' ORDER BY depth DESC) AS shape,
  group_concat(
    label || iif(run_len > 1, ' x' || run_len, ''),
    ' ' ORDER BY depth DESC
  ) AS path
FROM _leak_hops
GROUP BY
  start_id;

SELECT
  coalesce(pr.name, 'pid=' || pr.pid) AS process_name,
  a.graph_sample_ts,
  a.activity_class,
  count() AS instance_count,
  ifnull(v.view_root_impl_count, 0) AS view_root_impl_count
FROM _leak_activities AS a
JOIN process AS pr ON a.upid = pr.id
LEFT JOIN _leak_view_roots AS v USING (upid, graph_sample_ts)
GROUP BY
  a.upid,
  a.graph_sample_ts,
  a.activity_class
ORDER BY
  a.upid,
  a.graph_sample_ts,
  instance_count DESC;

SELECT
  g.process_name,
  g.graph_sample_ts,
  g.activity_class,
  g.instance_count,
  g.example_object_id,
  p.path AS gc_root_path
FROM (
  SELECT
    coalesce(pr.name, 'pid=' || pr.pid) AS process_name,
    a.upid,
    a.graph_sample_ts,
    a.activity_class,
    count() AS instance_count,
    min(a.id) AS example_object_id
  FROM _leak_activities AS a
  JOIN _leak_paths AS p ON p.start_id = a.id
  JOIN process AS pr ON a.upid = pr.id
  GROUP BY
    a.upid,
    a.graph_sample_ts,
    a.activity_class,
    p.shape
) AS g
JOIN _leak_paths AS p ON p.start_id = g.example_object_id
ORDER BY
  g.upid,
  g.graph_sample_ts,
  g.activity_class,
  g.instance_count DESC;
