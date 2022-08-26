select section, service, count(line) as linecount from android_dumpstate
group by section, service;