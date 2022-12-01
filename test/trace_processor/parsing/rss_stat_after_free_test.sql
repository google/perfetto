SELECT
  pid,
  max(c.ts) AS last_rss,
  p.end_ts AS process_end
FROM counter c
JOIN process_counter_track t ON c.track_id = t.id
JOIN process p USING(upid)
GROUP BY upid;
