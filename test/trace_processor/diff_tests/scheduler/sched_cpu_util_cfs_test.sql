SELECT
  t.name,
  c.ts,
  c.value
FROM
  counter AS c
LEFT JOIN
  counter_track AS t
  ON c.track_id = t.id
WHERE
  name GLOB "Cpu ? Cap" OR name GLOB "Cpu ? Util" OR name GLOB "Cpu ? Nr Running"
ORDER BY ts;
