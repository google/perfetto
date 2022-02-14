SELECT
  ts,
  s.name,
  dur,
  cat,
  t.name,
  EXTRACT_ARG(arg_set_id, 'ret') AS ret,
  EXTRACT_ARG(arg_set_id, 'len') AS len
FROM
  slice AS s
LEFT JOIN
  track AS t
  ON s.track_id = t.id
WHERE
  t.name GLOB "Napi Gro Cpu *"
ORDER BY ts;
