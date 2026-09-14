-- Copyright (C) 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--      http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Extracts canonical dominator class paths per (upid, graph_sample_ts, path, heap_type)
-- WITHOUT [self_count] in node labels so that paths can be joined and diffed cleanly
-- across baseline and candidate traces even when object counts change.
INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_class_tree;

INCLUDE PERFETTO MODULE graphs.scan;

INCLUDE PERFETTO MODULE graphs.hierarchy;

CREATE OR REPLACE PERFETTO TABLE _diff_target_class_nodes AS
SELECT id
FROM _heap_graph_dominator_class_tree
WHERE
  self_size > 0
  OR self_count > 0;

CREATE OR REPLACE PERFETTO TABLE _diff_class_ancestor_ids AS
SELECT id
FROM _tree_reachable_ancestors_or_self!((
    SELECT id, parent_id FROM _heap_graph_dominator_class_tree
  ), (SELECT id FROM _diff_target_class_nodes));

CREATE OR REPLACE PERFETTO TABLE _diff_edge_field_names AS
WITH
  raw_fields AS (
    SELECT DISTINCT
      COALESCE(p_cls.deobfuscated_name, p_cls.name) AS parent_class_name,
      COALESCE(c_cls.deobfuscated_name, c_cls.name) AS child_class_name,
      CASE WHEN r.field_name LIKE '[%]' THEN '[*]' ELSE r.field_name END AS field_name
    FROM heap_graph_dominator_tree AS dt
    JOIN heap_graph_object AS c_obj ON dt.id = c_obj.id
    JOIN heap_graph_class AS c_cls ON c_obj.type_id = c_cls.id
    JOIN heap_graph_object AS p_obj ON dt.idom_id = p_obj.id
    JOIN heap_graph_class AS p_cls ON p_obj.type_id = p_cls.id
    JOIN heap_graph_reference AS r
      ON r.owner_id = p_obj.id
      AND r.owned_id = c_obj.id
    WHERE
      r.field_name IS NOT NULL
      AND r.field_name != ''
  )
SELECT
  parent_class_name,
  child_class_name,
  GROUP_CONCAT(field_name, '|') AS field_names
FROM (SELECT * FROM raw_fields ORDER BY field_name)
GROUP BY
  parent_class_name,
  child_class_name;

CREATE OR REPLACE PERFETTO TABLE _diff_clean_class_labels AS
SELECT
  t.id,
  t.parent_id,
  IFNULL(t.name, '[Unknown]') AS label,
  t.root_type,
  ef.field_names
FROM _heap_graph_dominator_class_tree AS t
JOIN _diff_class_ancestor_ids AS a ON t.id = a.id
LEFT JOIN _heap_graph_dominator_class_tree AS pt ON t.parent_id = pt.id
LEFT JOIN _diff_edge_field_names AS ef
  ON pt.name = ef.parent_class_name
  AND t.name = ef.child_class_name;

CREATE OR REPLACE PERFETTO TABLE _diff_clean_class_paths AS
SELECT id, path
FROM _graph_scan!(
  (
    SELECT l.parent_id AS source_node_id, l.id AS dest_node_id
    FROM _diff_clean_class_labels l
    WHERE l.parent_id IS NOT NULL
  ),
  (
    SELECT l.id,
    "[" || COALESCE(l.root_type, "ROOT") || "] " || l.label AS path
    FROM _diff_clean_class_labels l
    WHERE l.parent_id IS NULL
  ),
  (path),
  (
    SELECT
    t.id,
    t.path
    || CASE
    WHEN l.field_names IS NOT NULL THEN " (" || l.field_names || ")"
    ELSE ""
    END
    || " -> " || l.label AS path
    FROM $table t
    JOIN _diff_clean_class_labels l ON t.id = l.id
  )
);

SELECT
  COALESCE(pr.name, "pid=" || pr.pid) AS process_name,
  t.upid,
  t.graph_sample_ts,
  p.path,
  t.heap_type,
  t.name AS class_name,
  SUM(t.self_count) AS self_count,
  SUM(t.self_size) AS self_size
FROM _heap_graph_dominator_class_tree AS t
JOIN process AS pr ON t.upid = pr.id
JOIN _diff_clean_class_paths AS p ON t.id = p.id
GROUP BY
  t.upid,
  t.graph_sample_ts,
  p.path,
  t.heap_type,
  t.name
ORDER BY
  self_size DESC,
  self_count DESC;
