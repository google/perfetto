SELECT
  hgc.name AS class_name,
  hgo.self_size AS single_object_self_size,
  COUNT(*) AS occurrence_count
FROM heap_graph_object AS hgo
JOIN heap_graph_class AS hgc ON hgo.type_id = hgc.id
GROUP BY
  hgc.name,
  hgo.self_size
ORDER BY
  occurrence_count DESC;
