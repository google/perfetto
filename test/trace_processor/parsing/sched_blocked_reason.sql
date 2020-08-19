select ts, tid, EXTRACT_ARG(arg_set_id, 'io_wait') as io_wait
from instants
join thread on instants.ref = thread.utid
where instants.name = 'sched_blocked_reason'