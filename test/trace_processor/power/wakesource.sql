SELECT ts, dur, slice.name
FROM slice
JOIN track on slice.track_id = track.id
WHERE track.name GLOB 'Wakelock*'
ORDER BY ts
