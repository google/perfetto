select ts, value, name
from counter
inner join counter_track on counter.track_id = counter_track.id
where name = 'MemAvailable' and counter_track.type = 'counter_track'
limit 10
