SELECT ts, t.name, value, tid
FROM counter c
JOIN thread_counter_track t ON c.track_id = t.id
JOIN thread USING (utid)
ORDER BY ts;
