SELECT
  ts,
  s.name,
  dur,
  t.name
FROM
  slice AS s
  LEFT JOIN track AS t
  ON s.track_id = t.id
WHERE
  t.name GLOB "TCP stream#*"
ORDER BY ts;
