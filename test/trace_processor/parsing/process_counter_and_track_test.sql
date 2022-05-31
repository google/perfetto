select ts, pct.name, value, pid
from counter c
join process_counter_track pct on c.track_id = pct.id
join process using (upid)
order by ts;
