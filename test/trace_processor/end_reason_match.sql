select end_state, count(*)
from sched
where end_state MATCH 'D'
group by end_state
