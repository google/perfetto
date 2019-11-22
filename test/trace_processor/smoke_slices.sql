select depth, count(*) as count
from slice
group by ref_type, depth
order by ref_type, depth;
