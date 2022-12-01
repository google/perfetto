SELECT c.id, c.superclass_id, c.name, s.name AS superclass_name, c.location
FROM heap_graph_class c LEFT JOIN heap_graph_class s ON c.superclass_id = s.id;
