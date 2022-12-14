SELECT ts, pct.name, value, pid
FROM counter c
JOIN process_counter_track pct ON c.track_id = pct.id
JOIN process USING (upid)
ORDER BY ts;
