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

-- Retention hotspots: the top 5 places per heap dump where retained memory
-- fans out in the dominator class tree.
--
-- The dominator class tree groups objects by the class-name path from their
-- GC root. Every node's retained size (java + native) includes its subtree,
-- so ranking by retained size alone just returns the root and the funnel of
-- single-child nodes beneath it. Instead, nodes are ranked by "spread": the
-- retained size NOT explained by the node's single largest child. A high
-- spread means "this is where the memory actually lives / branches out",
-- which is what you want to look at first.
--
-- Columns, one row per hotspot (up to 5 per dump):
--   process_name, graph_sample_ts  identify the dump (a trace can hold several)
--   rank                           1 = largest spread within that dump
--   class_name, instance_count     the hotspot class-tree node
--   retained_bytes, retained_pct   java + native retained by the node
--   spread_bytes                   retained_bytes minus largest child
--   path                           class path from the root of the dominator
--                                  class tree, with instance counts. A root
--                                  labelled [MULTIPLE_ROOTS] is held by more
--                                  than one GC root (e.g. a linked list whose
--                                  nodes are referenced from several places).
--   top_children                   up to 3 largest children: class (count,
--                                  retained bytes)

INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_class_tree;

INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;

-- Retained size of each class-tree node = sum of the retained sizes of its
-- "entry" objects (objects whose immediate dominator is in a different node;
-- consecutive same-class objects such as linked-list nodes share one node).
CREATE OR REPLACE PERFETTO TABLE _hotspot_node_retained AS
SELECT
  h.path_hash,
  sum(d.dominated_size_bytes + d.dominated_native_size_bytes) AS retained_bytes
FROM _heap_graph_dominator_path_hashes AS h
JOIN heap_graph_dominator_tree AS d USING (id)
LEFT JOIN _heap_graph_dominator_path_hashes AS p
  ON p.id = d.idom_id
WHERE
  p.path_hash IS NULL
  OR p.path_hash != h.path_hash
GROUP BY
  h.path_hash;

CREATE OR REPLACE PERFETTO TABLE _hotspot_node AS
SELECT
  t.id,
  t.parent_id,
  t.upid,
  t.graph_sample_ts,
  t.name,
  t.root_type,
  t.self_count,
  r.retained_bytes
FROM _heap_graph_dominator_class_tree AS t
JOIN _hotspot_node_retained AS r ON r.path_hash = t.path_hash_stable;

CREATE OR REPLACE PERFETTO TABLE _hotspot_max_child AS
SELECT parent_id AS id, max(retained_bytes) AS max_child_bytes
FROM _hotspot_node
WHERE
  parent_id IS NOT NULL
GROUP BY
  parent_id;

CREATE OR REPLACE PERFETTO TABLE _hotspot_dump_total AS
SELECT upid, graph_sample_ts, sum(retained_bytes) AS total_bytes
FROM _hotspot_node
WHERE
  parent_id IS NULL
GROUP BY
  upid,
  graph_sample_ts;

CREATE OR REPLACE PERFETTO TABLE _hotspot_top AS
SELECT *
FROM (
  SELECT
    n.*,
    n.retained_bytes - ifnull(m.max_child_bytes, 0) AS spread_bytes,
    row_number() OVER (
      PARTITION BY
        n.upid,
        n.graph_sample_ts
      ORDER BY n.retained_bytes - ifnull(m.max_child_bytes, 0) DESC
    ) AS rank
  FROM _hotspot_node AS n
  LEFT JOIN _hotspot_max_child AS m USING (id)
)
WHERE
  rank <= 5;

-- Walk up from each hotspot to its root to build the class path.
CREATE OR REPLACE PERFETTO TABLE _hotspot_ancestors AS
WITH RECURSIVE
  up(hotspot_id, id, depth) AS (
    SELECT id, id, 0 FROM _hotspot_top
    UNION ALL
    SELECT up.hotspot_id, n.parent_id, up.depth + 1
    FROM up
    JOIN _hotspot_node AS n
      ON n.id = up.id
    WHERE
      n.parent_id IS NOT NULL
  )
SELECT
  up.hotspot_id,
  up.depth,
  iif(
    n.parent_id IS NULL,
    '[' || ifnull(n.root_type, 'MULTIPLE_ROOTS') || '] ',
    ''
  )
  || ifnull(n.name, '[Unknown]')
  || ' ['
  || n.self_count
  || ']' AS label
FROM up
JOIN _hotspot_node AS n USING (id);

CREATE OR REPLACE PERFETTO TABLE _hotspot_paths AS
SELECT hotspot_id, group_concat(label, ' -> ' ORDER BY depth DESC) AS path
FROM _hotspot_ancestors
GROUP BY
  hotspot_id;

CREATE OR REPLACE PERFETTO TABLE _hotspot_children AS
SELECT
  parent_id AS hotspot_id,
  group_concat(
    name || ' (' || self_count || ', ' || retained_bytes || ' B)',
    '; ' ORDER BY retained_bytes DESC
  ) AS top_children
FROM (
  SELECT
    c.*,
    row_number() OVER (PARTITION BY c.parent_id ORDER BY c.retained_bytes DESC) AS rn
  FROM _hotspot_node AS c
  JOIN _hotspot_top AS t ON c.parent_id = t.id
)
WHERE
  rn <= 3
GROUP BY
  parent_id;

SELECT
  coalesce(pr.name, 'pid=' || pr.pid) AS process_name,
  t.graph_sample_ts,
  t.rank,
  t.name AS class_name,
  t.self_count AS instance_count,
  t.retained_bytes,
  round(100.0 * t.retained_bytes / d.total_bytes, 1) AS retained_pct,
  t.spread_bytes,
  p.path,
  c.top_children
FROM _hotspot_top AS t
JOIN _hotspot_dump_total AS d USING (upid, graph_sample_ts)
JOIN process AS pr ON t.upid = pr.id
LEFT JOIN _hotspot_paths AS p ON p.hotspot_id = t.id
LEFT JOIN _hotspot_children AS c ON c.hotspot_id = t.id
ORDER BY
  t.upid,
  t.graph_sample_ts,
  t.rank;
