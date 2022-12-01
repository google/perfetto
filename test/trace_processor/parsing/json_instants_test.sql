SELECT
  slice.ts,
  slice.name AS slice_name,
  thread.tid,
  process.pid
FROM slice
JOIN track ON (slice.track_id = track.id)
LEFT JOIN thread_track ON (slice.track_id = thread_track.id)
LEFT JOIN thread ON (thread_track.utid = thread.utid)
LEFT JOIN process_track ON (slice.track_id = process_track.id)
LEFT JOIN process ON (process_track.upid = process.upid)
WHERE dur = 0;
