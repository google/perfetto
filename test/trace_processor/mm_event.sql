select ts, name, value, ref, ref_type
from counters
where name like 'mem.mm.%'
order by ts
limit 40
