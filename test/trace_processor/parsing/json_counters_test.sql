SELECT
  process_counter_track.name,
  counter.ts,
  counter.value
FROM counter
JOIN process_counter_track ON (counter.track_id = process_counter_track.id);
