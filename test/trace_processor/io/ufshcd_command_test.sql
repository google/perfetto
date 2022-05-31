SELECT
  ts,
  value
FROM
  counter AS c
  JOIN
  counter_track AS ct
  ON c.track_id = ct.id
WHERE
  ct.name = "UFS Command Count"
ORDER BY ts
