select ts, name, value, ref
from counters
where name = "oom_score_adj"
order by ts
limit 20
