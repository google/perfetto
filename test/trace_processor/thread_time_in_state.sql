SELECT
  ts,
  tid,
  EXTRACT_ARG(counter.arg_set_id, 'time_in_state_cpu_id') AS cpu,
  EXTRACT_ARG(counter.arg_set_id, 'freq') AS freq,
  CAST(value AS INT) time_ms
FROM counter
JOIN thread_counter_track ON (counter.track_id = thread_counter_track.id)
JOIN thread USING (utid)
WHERE thread_counter_track.name = 'time_in_state'
ORDER BY ts, tid, cpu, freq;
