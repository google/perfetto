SELECT ts, dur, slice.name
FROM slice
JOIN track on slice.track_id = track.id
WHERE track.name GLOB 'io.ufs.command.tag*'
ORDER BY ts
