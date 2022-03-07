SELECT
  ts,
  REPLACE(name, " Received KB", "") AS dev,
  EXTRACT_ARG(arg_set_id, 'cpu') AS cpu,
  EXTRACT_ARG(arg_set_id, 'len') AS len
FROM
  counter AS c
  LEFT JOIN
  counter_track AS t
  ON c.track_id = t.id
WHERE
  name GLOB "* Received KB"
ORDER BY ts;
