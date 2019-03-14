SELECT
  ts,
  lead(ts, 1, ts) OVER (PARTITION BY name ORDER BY ts) - ts AS dur,
  value
FROM counters
WHERE ref = 1;
