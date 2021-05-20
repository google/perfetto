select
  slice.ts,
  slice.name as slice_name,
  thread.tid,
  process.pid
from slice
join track on (slice.track_id = track.id)
left join thread_track on (slice.track_id = thread_track.id)
left join thread on (thread_track.utid = thread.utid)
left join process_track on (slice.track_id = process_track.id)
left join process on (process_track.upid = process.upid)
where dur = 0;