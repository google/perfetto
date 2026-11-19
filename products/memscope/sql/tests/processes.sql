-- trace: test/data/two_procs_two_dumps_and_smaps.pftrace
INCLUDE PERFETTO MODULE memscope.processes;

SELECT
  pid,
  process_name,
  heap_dumps,
  smaps_snapshots,
  native_dumps,
  score,
  (SELECT pid FROM process WHERE upid = memscope_best_process()) AS best_pid
FROM memscope_process_mem_stats
ORDER BY
  pid;
