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

-- Extracts per-callstack allocation metrics per (process_name, upid, heap_name, path)
-- for Java (com.android.art) and Native (libc.malloc) allocation profiles.
INCLUDE PERFETTO MODULE android.memory.heap_profile.summary_tree;

INCLUDE PERFETTO MODULE graphs.scan;

INCLUDE PERFETTO MODULE graphs.hierarchy;

CREATE OR REPLACE PERFETTO TABLE _target_alloc_nodes AS
SELECT id
FROM android_heap_profile_summary_tree
WHERE
  self_size > 0
  OR self_alloc_size > 0;

CREATE OR REPLACE PERFETTO TABLE _alloc_ancestor_ids AS
SELECT id
FROM _tree_reachable_ancestors_or_self!((
    SELECT id, parent_id FROM android_heap_profile_summary_tree
  ), (SELECT id FROM _target_alloc_nodes));

CREATE OR REPLACE PERFETTO TABLE _frame_labels AS
SELECT t.id, t.parent_id, IFNULL(t.name, "[Unknown]") AS label
FROM android_heap_profile_summary_tree AS t
JOIN _alloc_ancestor_ids AS a ON t.id = a.id;

CREATE OR REPLACE PERFETTO TABLE _callstack_paths AS
SELECT id, path
FROM _graph_scan!(
  (
    SELECT l.parent_id AS source_node_id, l.id AS dest_node_id
    FROM _frame_labels l
    WHERE l.parent_id IS NOT NULL
  ),
  (
    SELECT l.id, l.label AS path
    FROM _frame_labels l
    WHERE l.parent_id IS NULL
  ),
  (path),
  (
    SELECT t.id, t.path || " -> " || l.label AS path
    FROM $table t
    JOIN _frame_labels l ON t.id = l.id
  )
);

CREATE OR REPLACE PERFETTO TABLE _profile_alloc_metrics AS
SELECT
  a.upid,
  COALESCE(p.name, "pid=" || p.pid) AS process_name,
  a.heap_name,
  a.ts AS dump_ts,
  a.callsite_id,
  SUM(a.size) AS self_size,
  SUM(MAX(a.size, 0)) AS self_alloc_size,
  SUM(a.count) AS self_count,
  SUM(MAX(a.count, 0)) AS self_alloc_count
FROM heap_profile_allocation AS a
JOIN process AS p USING (upid)
GROUP BY
  a.upid,
  process_name,
  a.heap_name,
  a.ts,
  a.callsite_id;

SELECT
  m.process_name,
  m.upid,
  m.heap_name,
  m.dump_ts,
  p.path,
  t.name AS leaf_function,
  t.mapping_name,
  COALESCE(t.source_file, "") AS source_file,
  COALESCE(t.line_number, 0) AS line_number,
  SUM(m.self_size) AS self_size,
  SUM(m.self_alloc_size) AS self_alloc_size,
  SUM(m.self_count) AS self_count,
  SUM(m.self_alloc_count) AS self_alloc_count,
  MAX(t.cumulative_size) AS cumulative_size,
  MAX(t.cumulative_alloc_size) AS cumulative_alloc_size
FROM _profile_alloc_metrics AS m
JOIN _callstack_spc_forest AS f
  ON m.callsite_id = f.callsite_id
  AND f.is_leaf_function_in_callsite_frame
JOIN android_heap_profile_summary_tree AS t ON f.id = t.id
JOIN _callstack_paths AS p ON t.id = p.id
GROUP BY
  m.process_name,
  m.upid,
  m.heap_name,
  m.dump_ts,
  p.path,
  t.name,
  t.mapping_name,
  t.source_file,
  t.line_number
ORDER BY
  self_alloc_size DESC,
  self_size DESC;
