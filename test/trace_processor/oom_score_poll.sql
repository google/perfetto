select ts, name, value, ref
from counters
where name = "oom_score_adj"
limit 20
