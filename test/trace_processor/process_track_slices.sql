SELECT
  ts,
  dur,
  pid,
  slice.name as slice_name,
  process_track.name as track_name
FROM slice
INNER JOIN process_track ON slice.ref = process_track.id
INNER JOIN process USING (upid)
