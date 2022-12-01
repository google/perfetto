SELECT slice.ts, slice.dur, slice.name, slice.depth
FROM slice
JOIN thread_track ON (slice.track_id = thread_track.id)
JOIN thread USING (utid)
WHERE tid = 42;
