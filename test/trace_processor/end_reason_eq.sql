select end_state, count(*)
from sched
where end_state = 'D'
group by end_state
