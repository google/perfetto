select ts
from sched
inner join thread using(utid)
where tid = 23850
order by ts desc
limit 10
