select depth, count(*) as count from slices group by depth order by depth;
