SELECT ts, instant.name, process.pid, process.name
FROM instant
JOIN thread_track ON instant.track_id = thread_track.id
JOIN thread USING (utid)
JOIN process USING (upid);
