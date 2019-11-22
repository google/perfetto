select ts, name, value, ref as upid
from counters
where name = "oom_score_adj"
order by ts
limit 20
