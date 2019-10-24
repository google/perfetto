select ts, value
from counters
where name like "power.%"
limit 20
