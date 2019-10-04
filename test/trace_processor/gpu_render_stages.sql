SELECT track.name AS track_name, ts,dur, gpu_slice.name AS slice_name,
    depth, gpu_slice.arg_set_id, flat_key, string_value, gpu_slice.context_id,
    render_target, submission_id, hw_queue_id
FROM gpu_track
LEFT JOIN track USING (id)
INNER JOIN gpu_slice on gpu_track.id=gpu_slice.track_id
LEFT JOIN args ON gpu_slice.arg_set_id = args.arg_set_id
ORDER BY ts;
