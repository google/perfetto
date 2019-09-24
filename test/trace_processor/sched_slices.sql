SELECT ts, cpu, dur, ts_end, end_state, priority, tid, name
FROM sched JOIN thread ON sched.utid == thread.utid
ORDER BY cpu, sched.ts ASC;
