select count(*) as cnt from android_logs union all
select count(*) as cnt from android_logs where prio = 3 union all
select count(*) as cnt from android_logs where prio > 4 union all
select count(*) as cnt from android_logs where tag = 'screen_toggled' union all
select count(*) as cnt from android_logs where tag like '%_pss' union all
select count(*) as cnt from android_logs where msg like '%i2c_write%' union all
select count(*) as cnt from android_logs where ts >= 1510113924391 and ts < 1512610021879;