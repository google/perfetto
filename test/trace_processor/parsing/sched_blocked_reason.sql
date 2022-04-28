select ts, tid, EXTRACT_ARG(arg_set_id, 'io_wait') as io_wait
from legacy_instant
join thread USING (utid)
where legacy_instant.name = 'sched_blocked_reason'
order by ts