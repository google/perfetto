SELECT
  ts
FROM
  counter AS c
  LEFT JOIN
  counter_track AS t
  ON c.track_id = t.id
WHERE
  name GLOB "Kfree Skb"
ORDER BY ts;