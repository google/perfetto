WITH
initial AS (SELECT
    (SELECT count(*) FROM android_logs) AS cnt,
    ts, prio, tag, msg FROM android_logs
  ORDER BY ts ASC
  LIMIT 100
),
latest AS (SELECT
    (SELECT count(*) FROM android_logs) AS cnt,
    ts, prio, tag, msg FROM android_logs
  ORDER BY ts DESC
  LIMIT 100
)
SELECT * FROM initial UNION ALL SELECT * FROM latest;
