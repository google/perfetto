with track_with_name as (
  select
    COALESCE(
      t1.name, 
      'thread=' || thread.name,
      'process=' || process.name,
      'tid=' || thread.tid,
      'pid=' || process.pid
    ) as full_name,
    *
  from track t1
  left join thread_track t2 using (id)
  left join thread using (utid)
  left join process_track t3 using (id)
  left join process on t3.upid=process.id
  order by id
)
select t1.full_name as name, t2.full_name as parent_name
from track_with_name t1
left join track_with_name t2 on t1.parent_id=t2.id
order by 1, 2;

