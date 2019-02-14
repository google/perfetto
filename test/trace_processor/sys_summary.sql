select name, count(*)
from slices
where name LIKE "sys_%"
group by name
