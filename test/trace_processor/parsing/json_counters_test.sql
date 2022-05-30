select
  process_counter_track.name,
  counter.ts,
  counter.value
from counter
join process_counter_track on (counter.track_id = process_counter_track.id);