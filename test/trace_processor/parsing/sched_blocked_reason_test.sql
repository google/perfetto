SELECT ts, tid, io_wait
FROM thread_state
JOIN thread USING (utid)
WHERE state = 'D'
ORDER BY ts;
