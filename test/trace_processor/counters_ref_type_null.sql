select id, counter_id, ts, value, arg_set_id, name, ref, ref_type from counters
where name = 'MemAvailable' and ref_type is null
limit 10
