select tid, pid, process.name as pname, thread.name as tname
from thread
left join process using(upid)
where tid > 0
order by utid