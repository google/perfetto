SELECT ts
FROM counter
WHERE
  ts > 72563651549 AND
  track_id = (
    SELECT t.id
    FROM process_counter_track t
    JOIN process p USING (upid)
    WHERE
      t.name = 'Heap size (KB)'
      AND p.pid = 1204
  ) AND
  value != 17952.000000
LIMIT 20
