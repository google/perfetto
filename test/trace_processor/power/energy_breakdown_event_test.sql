SELECT ts, value
FROM counter
JOIN energy_counter_track ON counter.track_id = energy_counter_track.id
ORDER BY ts
