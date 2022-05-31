select count(*) as count, 'BothProcesses' as description
from experimental_flamegraph
where
  upid_group = (
    select group_concat(distinct upid)
    from perf_sample join thread t using (utid) join process p using (upid)
  )
  and profile_type = 'perf'
  and ts <= 7689491063351
  and size > 0
union all
select count(*) as count, 'FirstProcess' as description
from experimental_flamegraph
  join process using (upid)
where pid = 1728
  and profile_type = 'perf'
  and ts <= 7689491063351
  and size > 0
union all
select count(*) as count, 'SecondProcess' as description
from experimental_flamegraph
  join process using (upid)
where pid = 703
  and profile_type = 'perf'
  and ts <= 7689491063351
  and size > 0;
