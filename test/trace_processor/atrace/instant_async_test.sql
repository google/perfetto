SELECT
  process.name AS process_name,
  process_track.name AS track_name,
  instant.name AS instant_name,
  ts
FROM slice instant
JOIN process_track ON instant.track_id = process_track.id
JOIN process USING (upid)
WHERE dur = 0;
