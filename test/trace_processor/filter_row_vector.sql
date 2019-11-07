SELECT ts FROM counter_values
WHERE
  ts > 72563651549 AND
  counter_id = (
    SELECT d.counter_id
    FROM counter_definitions d
    INNER JOIN process p on d.ref = p.upid
    WHERE
      d.name = 'Heap size (KB)'
      AND d.ref_type = 'upid'
      AND p.pid = 1204
  ) AND
  value != 17952.000000
LIMIT 20
