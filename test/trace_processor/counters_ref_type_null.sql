select ts, value, name, ref, ref_type from counters
where name = 'MemAvailable' and ref_type is null
limit 10
