SELECT "ts","dur","ref","ref_type","name","depth", internal_slice.arg_set_id, "flat_key",
       "string_value", "context_id", "render_target", "submission_id", "hw_queue_id" FROM internal_slice
INNER JOIN gpu_slice USING(slice_id)
LEFT JOIN args ON internal_slice.arg_set_id = args.arg_set_id
ORDER BY "ts";
