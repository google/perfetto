select ts, cast(value as integer) as int_value
from counters
where name like 'gpu_counter%'