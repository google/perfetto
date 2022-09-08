select slice.ts, slice.dur, slice.name, slice.depth
from slice
join thread_track on (slice.track_id = thread_track.id)
join thread using (utid)
where tid = 42
