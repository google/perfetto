-- Largest idle gaps on the GPU timeline.
--
-- After merging all GPU activity intervals into a single busy timeline, the
-- gaps between consecutive busy intervals are the windows where the device had
-- no work resident. These gaps are where you look UPSTREAM for the cause:
-- submission/launch latency, host-side synchronization or waits, device memory
-- allocation, host-device copies, or host-side work starving the device.
--
-- No parameters; operates on the whole trace. Returns the top idle gaps as
-- (gap_start_rel_ns, gap_dur_ns), where gap_start_rel_ns is relative to
-- trace_start(). To attribute a gap to host work, take a gap's
-- [gap_start, gap_start + gap_dur] window and run the host-attribution query
-- in timeline_occupancy.md.
--
-- NOTE: this merges across all GPUs into one timeline. For a multi-GPU trace,
-- scope to one device by adding a ugpu filter (see timeline_occupancy.md).

INCLUDE PERFETTO MODULE intervals.overlap;

CREATE PERFETTO TABLE _gpu_busy AS
SELECT ts, dur, ts + dur AS te
FROM interval_merge_overlapping!((SELECT ts, dur FROM gpu_slice WHERE dur > 0), 0);

SELECT ts - trace_start() AS gap_start_rel_ns, next_ts - ts AS gap_dur_ns
FROM (SELECT te AS ts, LEAD(ts) OVER (ORDER BY ts) AS next_ts FROM _gpu_busy)
WHERE
  next_ts - ts > 0
ORDER BY
  gap_dur_ns DESC
LIMIT 10;
