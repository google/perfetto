SELECT ef.*
FROM experimental_flamegraph ef
JOIN process USING (upid)
WHERE pid = 1728
  AND profile_type = 'perf'
  AND ts <= 7689491063351
LIMIT 10;
