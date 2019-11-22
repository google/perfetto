SELECT ts, instants.name, thread.name, thread.tid
FROM instants
JOIN thread ON instants.ref = thread.utid
WHERE instants.name = 'sched_waking'
ORDER BY ts
