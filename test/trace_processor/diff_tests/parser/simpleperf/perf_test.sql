WITH
  counter_delta_base AS (
    SELECT
      *,
      LAG(value) OVER (PARTITION BY track_id ORDER BY ts) AS lag_value
    FROM counter
  ),
  counter_delta AS (
    SELECT
      id,
      type,
      ts,
      track_id,
      IIF(lag_value IS NULL, value, value - lag_value) AS delta,
      arg_set_id
    FROM counter_delta_base
  )
SELECT
  CAST(SUM(c.delta) AS INTEGER) AS event_count,
  thread.name AS command,
  pid,
  tid,
  spm.name AS shared_object,
  IIF(
    spf.name IS NOT NULL AND spf.name <> '',
    spf.name,
    format(
      '%s[+%x]',
      -- substring after last /
      replace(spm.name, rtrim(spm.name, replace(spm.name, '/', '')), ''),
      spf.rel_pc)) AS symbol
FROM counter_delta AS c, perf_counter_track AS t
ON c.track_id = t.id,
perf_sample AS s
ON c.ts = s.ts AND t.perf_session_id = s.perf_session_id AND t.cpu = s.cpu,
thread USING (utid),
process USING (upid),
stack_profile_callsite AS spc ON (s.callsite_id = spc.id),
stack_profile_frame AS spf ON (spc.frame_id = spf.id),
stack_profile_mapping AS spm
ON (spf.mapping = spm.id)
WHERE
  s.cpu IN (2, 6, 7)
GROUP BY command, pid, tid, shared_object, symbol
ORDER BY event_count DESC;
