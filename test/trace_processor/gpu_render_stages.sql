SELECT track.name AS track_name, ts,dur, slice.name AS slice_name,
    depth, slice.arg_set_id, flat_key, string_value, gpu_slice.context_id,
    render_target, submission_id, hw_queue_id
FROM gpu_track
LEFT JOIN track USING (id)
LEFT JOIN slice ON gpu_track.id=slice.ref
INNER JOIN gpu_slice USING(slice_id)
LEFT JOIN args ON slice.arg_set_id = args.arg_set_id
ORDER BY ts;
