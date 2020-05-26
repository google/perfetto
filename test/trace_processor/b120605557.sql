select count(*)
from counter
inner join counter_track on counter_track.id = counter.track_id
