select
  ts,
  thread.tid as pid,
  EXTRACT_ARG(arg_set_id, 'function') as func
from instant
join thread on instant.ref = thread.utid
where instant.name = 'sched_blocked_reason';