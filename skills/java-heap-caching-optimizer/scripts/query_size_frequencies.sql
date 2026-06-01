SELECT
    hgc.name AS class_name,
    hgo.self_size as single_object_self_size,
    COUNT(*) AS occurrence_count
  FROM heap_graph_object hgo
  JOIN heap_graph_class hgc ON hgo.type_id = hgc.id
  GROUP BY hgc.name, hgo.self_size
ORDER BY occurrence_count DESC