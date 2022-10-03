SELECT ts, value
FROM counter
JOIN uid_counter_track ON counter.track_id = uid_counter_track.id
ORDER BY ts
