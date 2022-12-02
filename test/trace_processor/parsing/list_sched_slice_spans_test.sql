SELECT ts, dur, tid
FROM sched
JOIN thread USING(utid)
ORDER BY ts;
