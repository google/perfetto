select id, ts, name, value, ref, ref_type, arg_set_id
from counters
where name like 'mem.mm.%'
order by ts
limit 40
