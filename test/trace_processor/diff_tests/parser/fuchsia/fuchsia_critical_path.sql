INCLUDE PERFETTO MODULE sched.thread_executing_span;

SELECT
  root_utid,
  root_id,
  id,
  ts,
  dur,
  utid
FROM _thread_executing_span_critical_path(197, trace_start(), trace_dur())
LIMIT 10;
