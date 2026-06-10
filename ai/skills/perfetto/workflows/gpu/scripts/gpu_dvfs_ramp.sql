-- DVFS ramp latency after idle->busy transitions.
--
-- When a GPU wakes from idle, its clock is at a low (floor) state and the DVFS
-- governor must ramp it up. Until it reaches a healthy clock, the work that
-- triggered the wake runs slow. This tax is invisible to a busy/idle view and
-- to a steady-state frequency average -- it only shows up right after each
-- idle->busy edge. Bursty workloads (UI rendering, inference serving) pay it
-- repeatedly.
--
-- For every idle->busy edge (the start of a merged busy interval) where the
-- clock is below target, this reports how long until the clock first reaches
-- the target. Edges already at/above target are skipped (no ramp needed).
--
-- target = 90% of the per-GPU max OBSERVED frequency. To change it, edit the
-- 0.9 factor below.
--
-- Columns (worst ramps first):
--   edge_rel_ns       : edge timestamp, relative to trace start.
--   idle_gap_ns       : idle gap immediately before the edge; NULL for the
--                       first burst (cold start / trace boundary). A larger gap
--                       means the clock had more time to drop.
--   freq_at_edge_mhz  : clock at the moment work resumed.
--   target_mhz        : 90% of fmax.
--   ramp_ns           : time from the edge until the clock first reaches target.
--   completed         : 1 if target was reached before the burst ended; 0 if the
--                       burst ended still ramping (ramp_ns is then a lower bound:
--                       edge..burst end).

INCLUDE PERFETTO MODULE counters.intervals;

INCLUDE PERFETTO MODULE intervals.overlap;

CREATE PERFETTO TABLE _gpu_freq AS
SELECT f.id, f.ts, f.dur, f.value AS freq_khz, gct.ugpu
FROM counter_leading_intervals!((
    SELECT c.id, c.ts, c.track_id, c.value
    FROM counter AS c
    JOIN gpu_counter_track AS gct ON gct.id = c.track_id
    WHERE gct.name = 'gpufreq'
  )) AS f
JOIN gpu_counter_track AS gct
  ON gct.id = f.track_id;

CREATE PERFETTO TABLE _gpu_busy AS
SELECT
  ROW_NUMBER() OVER (ORDER BY ugpu, ts) AS id,
  ugpu,
  ts,
  dur,
  ts + dur AS te
FROM interval_merge_overlapping_partitioned!((
    SELECT
      s.ts,
      s.dur,
      IFNULL(EXTRACT_ARG(t.dimension_arg_set_id, 'ugpu'), 0) AS ugpu
    FROM gpu_slice AS s
    JOIN gpu_track AS t ON s.track_id = t.id
    WHERE
      s.dur > 0
  ), (ugpu));

CREATE PERFETTO TABLE _fmax AS
SELECT ugpu, MAX(freq_khz) AS fmax_khz FROM _gpu_freq GROUP BY ugpu;

-- One row per idle->busy edge, with the clock at the edge and the preceding gap.
CREATE PERFETTO TABLE _edges AS
SELECT
  b.ugpu,
  b.ts AS edge_ts,
  b.te AS busy_end,
  b.ts - LAG(b.te) OVER (PARTITION BY b.ugpu ORDER BY b.ts) AS idle_gap_ns,
  (
    SELECT f.freq_khz
    FROM _gpu_freq AS f
    WHERE
      f.ugpu = b.ugpu
      AND f.ts <= b.ts
      AND f.ts + f.dur > b.ts
  ) AS freq_at_edge_khz,
  (SELECT fmax_khz FROM _fmax WHERE ugpu = b.ugpu) AS fmax_khz
FROM _gpu_busy AS b;

SELECT
  e.ugpu AS gpu,
  IFNULL(g.name, 'GPU ' || e.ugpu) AS gpu_name,
  e.edge_ts - trace_start() AS edge_rel_ns,
  e.idle_gap_ns,
  e.freq_at_edge_khz / 1000 AS freq_at_edge_mhz,
  CAST(0.9 * e.fmax_khz AS INT) / 1000 AS target_mhz,
  IFNULL(
    (
      SELECT MIN(f.ts)
      FROM _gpu_freq AS f
      WHERE
        f.ugpu = e.ugpu
        AND f.freq_khz >= 0.9 * e.fmax_khz
        AND f.ts >= e.edge_ts
        AND f.ts < e.busy_end
    ),
    e.busy_end
  )
  - e.edge_ts AS ramp_ns,
  CASE
    WHEN (
      SELECT MIN(f.ts)
      FROM _gpu_freq AS f
      WHERE
        f.ugpu = e.ugpu
        AND f.freq_khz >= 0.9 * e.fmax_khz
        AND f.ts >= e.edge_ts
        AND f.ts < e.busy_end
    ) IS NOT NULL THEN 1
    ELSE 0
  END AS completed
FROM _edges AS e
LEFT JOIN gpu AS g ON g.ugpu = e.ugpu
WHERE
  e.freq_at_edge_khz < 0.9 * e.fmax_khz
ORDER BY
  ramp_ns DESC
LIMIT 20;
