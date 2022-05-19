select
  ts,
  thread.tid as pid,
  blocked_function as func
from thread_state
join thread USING (utid)
where state = 'D'
order by ts
