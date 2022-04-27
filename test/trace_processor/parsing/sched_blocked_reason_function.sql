select
  ts,
  thread.tid as pid,
  EXTRACT_ARG(arg_set_id, 'function') as func
from legacy_instant
join thread USING (utid)
where legacy_instant.name = 'sched_blocked_reason'
order by ts