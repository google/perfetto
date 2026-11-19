-- trace: test/data/two_procs_two_dumps_and_smaps.pftrace
INCLUDE PERFETTO MODULE memscope.java;

SELECT
  p.pid,
  g.type_name,
  g.first_size_bytes,
  g.last_size_bytes,
  g.delta_size_bytes,
  g.delta_obj_count
FROM memscope_java_class_growth AS g
JOIN process AS p USING (upid)
ORDER BY
  p.pid,
  g.delta_size_bytes DESC,
  g.type_name;
