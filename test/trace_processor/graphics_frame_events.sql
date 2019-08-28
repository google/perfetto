select ts, dur, name, key, string_value as layer_name
from internal_slice
left join args
on internal_slice.arg_set_id=args.arg_set_id and (args.key='layer_name')
