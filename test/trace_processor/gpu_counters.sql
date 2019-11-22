select "ts","value","name","gpu_id","description","unit"
from counter
join gpu_counter_track
  on counter.track_id = gpu_counter_track.id
order by "ts";
