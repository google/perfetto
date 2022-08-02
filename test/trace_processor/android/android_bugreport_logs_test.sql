select
  (select count(*) from android_logs) as cnt,
  ts,
  prio,
  tag,
  msg
from android_logs
order by ts desc
limit 10;
