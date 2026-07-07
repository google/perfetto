INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_class_tree;

INCLUDE PERFETTO MODULE graphs.scan;

INCLUDE PERFETTO MODULE graphs.hierarchy;

-- A trace can hold more than one heap dump: several processes, or the same
-- process sampled repeatedly. Each dump is identified by its (upid,
-- graph_sample_ts) pair and forms its own dominator forest in
-- _heap_graph_dominator_class_tree. Pick the largest-self_size class node
-- *per dump* (not a single global maximum) so every dump gets triaged.
CREATE OR REPLACE PERFETTO TABLE _top_class_nodes AS
SELECT id
FROM (
  SELECT
    id,
    ROW_NUMBER() OVER (
      PARTITION BY
        upid,
        graph_sample_ts
      ORDER BY self_size DESC
    ) AS rn
  FROM _heap_graph_dominator_class_tree
)
WHERE
  rn = 1;

CREATE OR REPLACE PERFETTO TABLE _class_ancestor_ids AS
SELECT id
FROM _tree_reachable_ancestors_or_self!((
    SELECT id, parent_id FROM _heap_graph_dominator_class_tree
  ), (SELECT id FROM _top_class_nodes));

CREATE OR REPLACE PERFETTO TABLE _class_labels AS
SELECT
  t.id,
  t.parent_id,
  IFNULL(t.name, "[Unknown]") || " [" || t.self_count || "]" AS label,
  t.root_type
FROM _heap_graph_dominator_class_tree AS t
JOIN _class_ancestor_ids AS a ON t.id = a.id;

CREATE OR REPLACE PERFETTO TABLE _class_paths AS
SELECT id, path
FROM _graph_scan!(
  (
    SELECT l.parent_id AS source_node_id, l.id AS dest_node_id
    FROM _class_labels l
    WHERE l.parent_id IS NOT NULL
  ),
  (
    SELECT l.id,
    "[" || COALESCE(l.root_type, "ROOT") || "] " || l.label AS path
    FROM _class_labels l
    WHERE l.parent_id IS NULL
  ),
  (path),
  (
    SELECT t.id, t.path || " -> " || l.label AS path
    FROM $table t
    JOIN _class_labels l ON t.id = l.id
  )
);

SELECT
  COALESCE(pr.name, "pid=" || pr.pid) AS process_name,
  t.graph_sample_ts,
  p.path,
  t.name AS class_name,
  t.self_size
FROM _top_class_nodes AS n
JOIN _heap_graph_dominator_class_tree AS t ON n.id = t.id
JOIN process AS pr ON t.upid = pr.id
LEFT JOIN _class_paths AS p ON n.id = p.id
ORDER BY
  t.self_size DESC;
