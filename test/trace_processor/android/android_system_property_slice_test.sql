select t.id, t.type, t.name, s.id, s.ts, s.dur, s.type, s.name
from track t join slice s on s.track_id = t.id
where t.name = 'DeviceStateChanged';
