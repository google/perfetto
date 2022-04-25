SELECT
  gpu_track.name,
  ts,
  dur,
  slice.name,
  flat_key,
  int_value,
  string_value
FROM
  gpu_track
  JOIN slice
  ON slice.track_id = gpu_track.id
  JOIN args
  ON slice.arg_set_id = args.arg_set_id
ORDER BY ts
