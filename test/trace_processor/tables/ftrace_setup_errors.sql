SELECT value FROM stats WHERE name = 'ftrace_setup_errors'
UNION ALL
SELECT str_value FROM metadata WHERE name = 'ftrace_setup_errors'
