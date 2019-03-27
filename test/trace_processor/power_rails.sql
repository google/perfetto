select name, AVG(value), COUNT(*)
from counters
where name like "power.%"
group by name
limit 20