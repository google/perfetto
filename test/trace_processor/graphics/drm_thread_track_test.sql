SELECT
  utid,
  ts,
  dur,
  slice.name,
  flat_key,
  int_value,
  string_value
FROM
  thread_track
  JOIN slice
  ON slice.track_id = thread_track.id
  JOIN args
  ON slice.arg_set_id = args.arg_set_id
ORDER BY ts
