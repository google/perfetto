SELECT
t.name,
tid,
c.ts,
c.value
FROM counter c
JOIN thread_counter_track t ON c.track_id = t.id
JOIN thread USING (utid)
ORDER BY c.ts;
