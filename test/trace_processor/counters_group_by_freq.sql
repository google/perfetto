SELECT
  value,
  sum(dur) as dur_sum
FROM (
  SELECT value,
  lead(ts) OVER (PARTITION BY name, ref ORDER BY ts) - ts AS dur
  FROM counters
)
WHERE value > 0
GROUP BY value
ORDER BY dur_sum desc
