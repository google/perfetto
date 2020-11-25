select ts, t.name, value, tid
from counter c
join thread_counter_track t on c.track_id = t.id
join thread using (utid)
order by ts;