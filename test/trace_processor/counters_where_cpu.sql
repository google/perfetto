SELECT
  ts,
  lead(ts, 1, ts) OVER (PARTITION BY name ORDER BY ts) - ts AS dur,
  value
FROM counter c
INNER JOIN cpu_counter_track t ON t.id = c.track_id
WHERE cpu = 1;
