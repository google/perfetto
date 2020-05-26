create view freq_view as
  select
    ts,
    lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts as dur,
    cpu,
    name as freq_name,
    value as freq_value
  from counter
  inner join cpu_counter_track
    on counter.track_id = cpu_counter_track.id
  where name = 'cpufreq';

create view idle_view
  as select
    ts,
    lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts as dur,
    cpu,
    name as idle_name,
    value as idle_value
  from counter
  inner join cpu_counter_track
    on counter.track_id = cpu_counter_track.id
  where name = 'cpuidle';

create virtual table freq_idle
  using span_join(freq_view PARTITIONED cpu, idle_view PARTITIONED cpu)

create virtual table window_freq_idle using window;

create virtual table span_freq_idle
  using span_join(freq_idle PARTITIONED cpu, window_freq_idle)

update window_freq_idle
  set
    window_start=(select min(ts) from sched),
    window_dur=(select max(ts) - min(ts) from sched),
    quantum=1000000
  where rowid = 0

create view counter_view
  as select
    ts,
    dur,
    quantum_ts,
    cpu,
    case idle_value
      when 4294967295 then "freq"
      else "idle"
    end as name,
    case idle_value
      when 4294967295 then freq_value
      else idle_value
    end as value
  from span_freq_idle

select cpu, name, value, sum(dur) from counter_view group by cpu, name, value
