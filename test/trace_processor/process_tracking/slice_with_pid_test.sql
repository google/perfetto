SELECT s.name, dur, tid, pid
FROM slice s
JOIN thread_track t ON s.track_id = t.id
JOIN thread USING(utid)
LEFT JOIN process USING(upid);
