select ts, tid, io_wait
from thread_state
join thread using (utid)
where state = 'D'
order by ts
