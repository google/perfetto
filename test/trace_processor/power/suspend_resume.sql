SELECT
  s.ts,
  s.dur,
  s.name AS action
FROM
  slice AS s
  JOIN
  track AS t
  ON s.track_id = t.id
WHERE
  t.name = 'Suspend/Resume Latency'
ORDER BY s.ts;
