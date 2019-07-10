select ref_type, depth, count(*) as count from slices group by ref_type, depth order by ref_type, depth;
