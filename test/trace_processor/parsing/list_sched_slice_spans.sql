select ts, dur, tid
from sched
join thread using(utid)
order by ts