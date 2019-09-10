SELECT ts FROM counter_values
WHERE
  ts > 72563651549 AND
  counter_id = 7 AND
  value != 17952.000000
LIMIT 20
