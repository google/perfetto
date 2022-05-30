select ef.*
from experimental_flamegraph ef
  join process using (upid)
where pid = 1728
  and profile_type = 'perf'
  and ts <= 7689491063351
limit 10;
