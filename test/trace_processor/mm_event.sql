select ts, name, value, upid
from counter
inner join process_counter_track
  on counter.track_id = process_counter_track.id
where name like 'mem.mm.%'
order by ts
limit 40
