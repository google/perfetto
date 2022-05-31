SELECT
  ts,
  value,
  EXTRACT_ARG(arg_set_id, 'protocol') AS prot
FROM
  counter AS c
  LEFT JOIN
  counter_track AS t
  ON c.track_id = t.id
WHERE
  name GLOB "Kfree Skb IP Prot"
ORDER BY ts;
