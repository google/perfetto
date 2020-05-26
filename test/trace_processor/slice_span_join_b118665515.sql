create virtual table window_8 using window;

create virtual table span_8 using span_join(sched PARTITIONED cpu, window_8);

update window_8 set window_start=81473010031230, window_dur=19684693341, quantum=10000000 where rowid = 0;

select quantum_ts as bucket, sum(dur)/cast(10000000 as float) as utilization from span_8 where cpu = 7 and utid != 0 group by quantum_ts;
