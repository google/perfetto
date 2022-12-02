SELECT count(*) AS count, 'BothProcesses' AS description
FROM experimental_flamegraph
WHERE
  upid_group = (
    SELECT group_concat(DISTINCT upid)
    FROM perf_sample JOIN thread t USING (utid) JOIN process p USING (upid)
  )
  AND profile_type = 'perf'
  AND ts <= 7689491063351
  AND size > 0
UNION ALL
SELECT count(*) AS count, 'FirstProcess' AS description
FROM experimental_flamegraph
JOIN process USING (upid)
WHERE pid = 1728
  AND profile_type = 'perf'
  AND ts <= 7689491063351
  AND size > 0
UNION ALL
SELECT count(*) AS count, 'SecondProcess' AS description
FROM experimental_flamegraph
JOIN process USING (upid)
WHERE pid = 703
  AND profile_type = 'perf'
  AND ts <= 7689491063351
  AND size > 0;
