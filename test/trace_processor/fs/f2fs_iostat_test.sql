SELECT
  name,
  ts,
  value
FROM
  counter AS c
  JOIN
  counter_track AS ct
  ON c.track_id = ct.id
ORDER BY name,ts
