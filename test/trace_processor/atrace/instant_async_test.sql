SELECT
  process.name AS process_name,
  process_track.name as track_name,
  instant.name as instant_name,
  ts
FROM slice instant
JOIN process_track ON instant.track_id = process_track.id
JOIN process USING (upid)
WHERE dur = 0;
