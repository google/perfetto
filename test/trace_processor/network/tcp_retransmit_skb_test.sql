SELECT
  ts,
  s.name,
  dur
FROM
  slice AS s
  LEFT JOIN track AS t
  ON s.track_id = t.id
WHERE
  t.name = "TCP Retransmit Skb"
ORDER BY ts;
