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

-- Finds what keeps reachable instances of <class_name> alive by grouping them
-- by their shortest strong-reference path from a GC root (similar to MAT's
-- "Merge Shortest Paths to GC Roots" and LeakCanary).
--
-- Replace <class_name> with a fully qualified class name, e.g.
-- com.example.Foo$Bar.
--
-- Returns up to 20 path groups per dump, ordered by total bytes (java + native):
--   process_name, graph_sample_ts, instances, java_bytes, native_bytes,
--   example_object_id, gc_root_path
--
-- gc_root_path reads from the GC root down to <class_name>:
--   [ROOT_TYPE] RootClass .field -> NextClass .field -> ... -> TargetClass
-- Consecutive same-class hops (such as linked-list nodes) are collapsed.

INCLUDE PERFETTO MODULE android.memory.heap_graph.class_tree;

WITH RECURSIVE
  path_groups AS MATERIALIZED (
    SELECT *
    FROM (
      SELECT
        o.upid,
        o.graph_sample_ts,
        count() AS instances,
        sum(max(o.self_size, 0)) AS java_bytes,
        sum(o.native_size) AS native_bytes,
        min(o.id) AS example_object_id,
        row_number() OVER (
          PARTITION BY
            o.upid,
            o.graph_sample_ts
          ORDER BY sum(max(o.self_size, 0) + o.native_size) DESC, count() DESC
        ) AS rank
      FROM heap_graph_object AS o
      INNER JOIN heap_graph_class AS c ON c.id = o.type_id
      INNER JOIN _heap_graph_path_hashes AS h ON h.id = o.id
      WHERE
        o.reachable
        AND coalesce(c.deobfuscated_name, c.name) = '<class_name>'
      GROUP BY
        o.upid,
        o.graph_sample_ts,
        h.path_hash
    )
    WHERE
      rank <= 20
  ),
  walk(target_id, id, depth) AS (
    SELECT example_object_id, example_object_id, 0 FROM path_groups
    UNION ALL
    SELECT walk.target_id, t.parent_id, walk.depth + 1
    FROM walk
    INNER JOIN _heap_graph_object_min_depth_tree AS t ON t.id = walk.id
    WHERE
      t.parent_id IS NOT NULL
      AND walk.depth < 200
  ),
  ancestors AS (
    SELECT
      w.target_id,
      w.id,
      w.depth,
      coalesce(c.deobfuscated_name, c.name) AS cls,
      o.root_type,
      lag(coalesce(c.deobfuscated_name, c.name)) OVER w AS prev_cls,
      lag(o.reference_set_id) OVER w AS prev_ref_set_id
    FROM walk AS w
    INNER JOIN heap_graph_object AS o ON o.id = w.id
    INNER JOIN heap_graph_class AS c ON c.id = o.type_id
    WINDOW
      w AS (PARTITION BY w.target_id ORDER BY w.depth DESC)
  ),
  hops AS (
    SELECT
      a.target_id,
      a.depth,
      a.cls,
      a.root_type,
      a.prev_cls,
      iif(
        a.prev_ref_set_id IS NULL
        OR a.prev_cls GLOB '*[]',
        NULL,
        regexp_extract(
          (
            SELECT
              min(nullif(coalesce(r.deobfuscated_field_name, r.field_name), ''))
            FROM heap_graph_reference AS r
            WHERE
              r.reference_set_id = a.prev_ref_set_id
              AND r.owned_id = a.id
          ),
          '[^.]+$'
        )
      ) AS field
    FROM ancestors AS a
    WHERE
      a.prev_cls IS NULL
      OR a.prev_cls != a.cls
  ),
  paths AS (
    SELECT
      target_id,
      group_concat(
        iif(root_type IS NOT NULL, '[' || root_type || '] ', '')
        || iif(
          prev_cls IS NULL,
          '',
          iif(field IS NOT NULL, '.' || field || ' -> ', '-> ')
        )
        || cls,
        ' ' ORDER BY depth DESC
      ) AS gc_root_path
    FROM hops
    GROUP BY
      target_id
  )
SELECT
  coalesce(pr.name, 'pid=' || pr.pid) AS process_name,
  g.graph_sample_ts,
  sum(g.instances) AS instances,
  sum(g.java_bytes) AS java_bytes,
  sum(g.native_bytes) AS native_bytes,
  min(g.example_object_id) AS example_object_id,
  p.gc_root_path
FROM path_groups AS g
INNER JOIN paths AS p ON p.target_id = g.example_object_id
INNER JOIN process AS pr ON pr.id = g.upid
GROUP BY
  g.upid,
  g.graph_sample_ts,
  p.gc_root_path
ORDER BY
  g.upid,
  g.graph_sample_ts,
  native_bytes + java_bytes DESC,
  instances DESC,
  example_object_id;
