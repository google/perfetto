select ts, tid
from instants
inner join thread on instants.ref = thread.utid
where instants.name = 'sched_wakeup'
limit 20
