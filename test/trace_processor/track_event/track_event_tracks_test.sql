WITH track_with_name AS (
  SELECT
    COALESCE(
      t1.name,
      'thread=' || thread.name,
      'process=' || process.name,
      'tid=' || thread.tid,
      'pid=' || process.pid
    ) AS full_name,
    *
  FROM track t1
  LEFT JOIN thread_track t2 USING (id)
  LEFT JOIN thread USING (utid)
  LEFT JOIN process_track t3 USING (id)
  LEFT JOIN process ON t3.upid=process.id
  ORDER BY id
)
SELECT t1.full_name AS name, t2.full_name AS parent_name,
       EXTRACT_ARG(t1.source_arg_set_id, 'has_first_packet_on_sequence')
           AS has_first_packet_on_sequence
FROM track_with_name t1
LEFT JOIN track_with_name t2 ON t1.parent_id=t2.id
ORDER BY 1, 2;
