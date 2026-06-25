INCLUDE PERFETTO MODULE android.memory.heap_profile.summary_tree;

INCLUDE PERFETTO MODULE graphs.scan;

INCLUDE PERFETTO MODULE graphs.hierarchy;

-- Find the top Java allocation site (leaf frame) by unreleased size
-- Note: This uses the collapsed summary tree which aggregates all heaps.
-- It assumes the trace is dominated by Java allocations (profiled with art heap).
CREATE OR REPLACE PERFETTO TABLE _top_alloc_nodes AS
SELECT id
FROM android_heap_profile_summary_tree
WHERE
  self_size > 0
ORDER BY
  self_size DESC
LIMIT 1;

-- Find all ancestors (the callstack)
CREATE OR REPLACE PERFETTO TABLE _alloc_ancestor_ids AS
SELECT id
FROM _tree_reachable_ancestors_or_self!((
    SELECT id, parent_id FROM android_heap_profile_summary_tree
  ), (SELECT id FROM _top_alloc_nodes));

-- Create labels for the frames
CREATE OR REPLACE PERFETTO TABLE _frame_labels AS
SELECT
  t.id,
  t.parent_id,
  IFNULL(t.name, "[Unknown]") || " [" || t.mapping_name || "]" AS label
FROM android_heap_profile_summary_tree AS t
JOIN _alloc_ancestor_ids AS a
  ON t.id = a.id;

-- Reconstruct the path (callstack)
CREATE OR REPLACE PERFETTO TABLE _callstack_paths AS
SELECT id, path
FROM _graph_scan!(
  (
    SELECT l.parent_id AS source_node_id, l.id AS dest_node_id
    FROM _frame_labels l
    WHERE l.parent_id IS NOT NULL
  ),
  (
    SELECT l.id,
    l.label AS path
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

-- Get the main profiled process name
CREATE OR REPLACE PERFETTO TABLE _main_process AS
SELECT COALESCE(name, "pid=" || pid) AS process_name
FROM process
WHERE
  upid
  = (
    SELECT upid
    FROM heap_profile_allocation
    GROUP BY
      upid
    ORDER BY
      sum(size) DESC
    LIMIT 1
  );

-- Select the final result, mapping to Java-style column names for compatibility
SELECT
  (SELECT process_name FROM _main_process) AS process_name,
  p.path AS path,
  t.name AS class_name, -- This will be the Java method name
  t.self_size AS self_size
FROM _top_alloc_nodes AS n
JOIN android_heap_profile_summary_tree AS t
  ON n.id = t.id
LEFT JOIN _callstack_paths AS p
  ON n.id = p.id;
