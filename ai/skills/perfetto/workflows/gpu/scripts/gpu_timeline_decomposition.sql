-- GPU timeline wall-clock decomposition: device-busy vs idle.
--
-- For each GPU, reports how much of wall-clock time had work resident on the
-- device versus sitting idle. This is the trace-level answer that a coarse
-- device "utilization percent" gauge cannot give: such gauges report only that
-- something is resident, not how the timeline decomposes or where the idle is.
--
-- "Busy" is the UNION of GPU activity intervals (work running concurrently on
-- multiple hardware queues is merged, not summed, so concurrency is never
-- double-counted). Covers all GPU render-stage activity, not just one category.
--
-- No parameters; operates on the whole trace. One row per GPU.
--   busy_pct_of_active : busy / (first .. last activity) span -> packing.
--   busy_pct_of_trace  : busy / whole-trace wall clock        -> idle outside
--                        the active span (setup, I/O, teardown) shows up here.

INCLUDE PERFETTO MODULE intervals.overlap;

CREATE PERFETTO TABLE _gpu_work AS
SELECT
  s.ts,
  s.dur,
  IFNULL(EXTRACT_ARG(t.dimension_arg_set_id, 'ugpu'), 0) AS ugpu
FROM gpu_slice AS s
JOIN gpu_track AS t ON s.track_id = t.id
WHERE
  s.dur > 0;

CREATE PERFETTO TABLE _gpu_busy AS
SELECT ugpu, ts, dur
FROM interval_merge_overlapping_partitioned!((
    SELECT ts, dur, ugpu FROM _gpu_work
  ), (ugpu));

SELECT
  k.ugpu AS gpu,
  IFNULL(g.name, 'GPU ' || k.ugpu) AS gpu_name,
  COUNT(*) AS activities,
  trace_end() - trace_start() AS trace_wall_ns,
  MAX(k.ts + k.dur) - MIN(k.ts) AS active_span_ns,
  (SELECT SUM(b.dur) FROM _gpu_busy AS b WHERE b.ugpu = k.ugpu) AS gpu_busy_ns,
  ROUND(
    100.0 * (SELECT SUM(b.dur) FROM _gpu_busy AS b WHERE b.ugpu = k.ugpu)
    / (MAX(k.ts + k.dur) - MIN(k.ts)),
    1
  ) AS busy_pct_of_active,
  ROUND(
    100.0 * (SELECT SUM(b.dur) FROM _gpu_busy AS b WHERE b.ugpu = k.ugpu)
    / (trace_end() - trace_start()),
    1
  ) AS busy_pct_of_trace
FROM _gpu_work AS k
LEFT JOIN gpu AS g ON g.ugpu = k.ugpu
GROUP BY
  k.ugpu,
  gpu_name
ORDER BY
  k.ugpu;
