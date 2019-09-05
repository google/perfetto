select ts, dur, name, key, frame_id, string_value as layer_name
from internal_slice
inner join gpu_slice using(slice_id)
left join args
on internal_slice.arg_set_id=args.arg_set_id and args.key='layer_name'
