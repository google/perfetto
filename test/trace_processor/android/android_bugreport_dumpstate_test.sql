SELECT section, service, count(line) AS linecount FROM android_dumpstate
GROUP BY section, service;
