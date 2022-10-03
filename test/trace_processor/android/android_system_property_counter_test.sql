select t.id, t.type, t.name, c.id, c.ts, c.type, c.value
from counter_track t join counter c on t.id = c.track_id
where name = 'ScreenState';
