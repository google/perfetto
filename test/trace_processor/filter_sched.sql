select ts, cpu, dur from sched
where
  cpu = 1 and
  dur > 50 and
  dur <= 100 and
  ts >= 100 and
  ts <= 400;
