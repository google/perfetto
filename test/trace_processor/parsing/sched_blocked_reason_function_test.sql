SELECT
  ts,
  thread.tid AS pid,
  blocked_function AS func
FROM thread_state
JOIN thread USING (utid)
WHERE state = 'D'
ORDER BY ts;
