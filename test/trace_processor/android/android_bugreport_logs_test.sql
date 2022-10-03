with
initial as
  (select
    (select count(*) from android_logs) as cnt,
    ts, prio, tag, msg from android_logs
    order by ts asc
    limit 100
),
latest as
  (select
    (select count(*) from android_logs) as cnt,
    ts, prio, tag, msg from android_logs
    order by ts desc
    limit 100
)
select * from initial union all select * from latest;
