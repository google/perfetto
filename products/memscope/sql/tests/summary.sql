-- trace: test/data/two_procs_two_dumps_and_smaps.pftrace
INCLUDE PERFETTO MODULE memscope.summary;

SELECT
  p.pid,
  t.snapshot_count,
  t.peak_bytes,
  t.first_bytes,
  t.last_bytes,
  t.delta_bytes,
  t.trend_bytes_per_hour
FROM memscope_process_memory_trend AS t
JOIN process AS p USING (upid)
ORDER BY
  p.pid;
