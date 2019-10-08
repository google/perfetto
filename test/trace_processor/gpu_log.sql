select scope, track.name as track_name, ts, dur, gpu_slice.name as slice_name,
    key, string_value as value
from gpu_track
left join track using (id)
left join gpu_slice on gpu_track.id=gpu_slice.track_id
left join args using (arg_set_id)
order by ts, slice_name, key
