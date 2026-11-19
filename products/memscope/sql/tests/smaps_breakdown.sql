-- trace: test/data/two_procs_two_dumps_and_smaps.pftrace
INCLUDE PERFETTO MODULE memscope.smaps;

SELECT
  p.pid,
  b.ts,
  b.category,
  b.rss_bytes,
  b.swap_bytes,
  b.anon_bytes,
  b.rss_swap_bytes,
  b.anon_swap_bytes
FROM memscope_smaps_category_breakdown AS b
JOIN process AS p USING (upid)
ORDER BY
  p.pid,
  b.ts,
  b.category;
