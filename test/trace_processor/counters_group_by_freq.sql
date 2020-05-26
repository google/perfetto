SELECT
  value,
  sum(dur) as dur_sum
FROM (
  SELECT value,
  lead(ts) OVER (PARTITION BY name, track_id ORDER BY ts) - ts AS dur
  FROM counter
  INNER JOIN counter_track ON counter.track_id = counter_track.id
)
WHERE value > 0
GROUP BY value
ORDER BY dur_sum desc
