SELECT track.name, slice.ts, slice.dur, slice.name
FROM slice JOIN track ON slice.track_id = track.id
WHERE track.name = 'mem.dma_buffer';
