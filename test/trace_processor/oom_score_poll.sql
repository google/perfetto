select ts, name, value, upid
from counter c
join process_counter_track t
  on c.track_id = t.id
where name = "oom_score_adj"
order by ts
limit 20
