SELECT t.id, t.type, t.name, c.id, c.ts, c.type, c.value
FROM counter_track t JOIN counter c ON t.id = c.track_id
WHERE name = 'ScreenState';
