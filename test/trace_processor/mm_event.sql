select ts, name, value, ref as upid
from counters
where name like 'mem.mm.%'
order by ts
limit 40
