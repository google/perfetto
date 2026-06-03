INCLUDE PERFETTO MODULE android.memory.heap_graph.class_summary_tree;

SELECT
  count(self_count) AS total_unique_paths_to_gc_root,
  name AS class_name,
  SUM(self_count) AS total_objects,
  sum(cumulative_size) AS total_objects_memory_consumption,
  MIN(self_size) AS single_object_self_size,
  MIN(cumulative_size) AS single_object_cumulative_size
FROM android_heap_graph_class_summary_tree
GROUP BY
  name
ORDER BY
  count(*) DESC;
