select count(*) as count, 'BothProcesses' as description from experimental_flamegraph
where upid_group = "30,41"
  and profile_type = 'perf'
  and ts <= 7689491063351
  and size > 0
union all
select count(*) as count, 'FirstProcess' as description from experimental_flamegraph
where upid = 30
  and profile_type = 'perf'
  and ts <= 7689491063351
  and size > 0
union all
select count(*) as count, 'SecondProcess' as description from experimental_flamegraph
where upid = 41
  and profile_type = 'perf'
  and ts <= 7689491063351
  and size > 0;
