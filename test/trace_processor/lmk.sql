select ts, process.pid
from instants
inner join process
on instants.ref = process.upid;