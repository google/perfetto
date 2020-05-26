SELECT
  ts,
  cpu,
  dur,
  end_state,
  priority,
  tid
FROM sched
JOIN thread USING(utid)
ORDER BY ts
LIMIT 10;
