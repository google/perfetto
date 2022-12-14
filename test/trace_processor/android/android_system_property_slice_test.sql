SELECT t.id, t.type, t.name, s.id, s.ts, s.dur, s.type, s.name
FROM track t JOIN slice s ON s.track_id = t.id
WHERE t.name = 'DeviceStateChanged';
