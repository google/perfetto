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
  ),
  named_counter AS (
    SELECT
      perf_session_id,
      ts,
      cpu,
      SUM(cast_int !(IIF(t.name = 'cpu-cycles', c.delta, 0)))
        AS cpu_cycles,
      SUM(cast_int !(IIF(t.name = 'instructions', c.delta, 0)))
        AS instructions,
      SUM(
        cast_int
          !(IIF(t.name NOT IN ('cpu-cycles', 'instructions'), c.delta, 0)))
        AS others
    FROM counter_delta AS c, perf_counter_track AS t
    ON (c.track_id = t.id)
    GROUP BY
      perf_session_id,
      ts,
      cpu
  )
SELECT
  SUM(c.cpu_cycles) AS cpu_cycles,
  SUM(c.instructions) AS instructions,
  -- Additional column (not present in simpleperf output) to validate that there
  -- are no other counters.
  SUM(c.others) AS others,
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
FROM
  named_counter AS c,
  perf_sample AS s
USING (perf_session_id, ts, cpu),
thread USING (utid),
process USING (upid),
stack_profile_callsite AS spc ON (s.callsite_id = spc.id),
stack_profile_frame AS spf ON (spc.frame_id = spf.id),
stack_profile_mapping AS spm
ON (spf.mapping = spm.id)
GROUP BY command, pid, tid, shared_object, symbol
ORDER BY cpu_cycles DESC;
