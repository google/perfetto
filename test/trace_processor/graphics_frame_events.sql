select scope, track.name as track_name, ts, dur, gpu_slice.name as slice_name,
    frame_id, key, string_value as layer_name
from gpu_track
left join track using (id)
left join gpu_slice on gpu_track.id=gpu_slice.track_id
left join args on gpu_slice.arg_set_id=args.arg_set_id and args.key='layer_name'
