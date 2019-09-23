select scope, track.name as track_name, ts, dur, slice.name as slice_name,
    frame_id, key, string_value as layer_name
from gpu_track
left join track using (id)
left join slice on gpu_track.id=slice.ref
left join gpu_slice using(slice_id)
left join args on slice.arg_set_id=args.arg_set_id and args.key='layer_name'
