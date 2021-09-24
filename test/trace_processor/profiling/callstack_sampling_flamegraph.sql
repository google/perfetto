select * from experimental_flamegraph
where upid = 30
  and profile_type = 'perf'
  and ts <= 7689491063351
limit 10;
