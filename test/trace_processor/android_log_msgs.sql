create view v1 as select tag, count(*) from android_logs group by tag order by 2 desc limit 5

create view v2 as select tag, count(*) from android_logs group by tag order by 2 asc limit 5;

create view v3 as select tag, count(*) from android_logs where msg like '%wakelock%' group by tag;

create view v4 as select msg, 1 from android_logs limit 10;

select * from v1 union all
select '-----', 0 union all
select * from v2 union all
select '-----', 0 union all
select * from v3 union all
select '-----', 0 union all
select * from v4;