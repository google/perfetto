select * from counters
where name = 'MemAvailable' and ref_type is null
limit 10
