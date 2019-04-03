select
  utid,
  tid,
  upid,
  pid,
  thread.name as threadName,
  process.name as processName,
  total_dur as totalDur
from
  thread
  left join process using(upid)
  left join (select upid, sum(dur) as total_dur
      from sched join thread using(utid)
      group by upid
    ) using(upid) group by utid, upid
order by total_dur desc, upid, utid
