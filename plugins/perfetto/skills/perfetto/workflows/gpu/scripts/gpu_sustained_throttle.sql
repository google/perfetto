-- Sustained GPU throttling: clock held below target while busy, after it had
-- already ramped up -- correlated with temperature and power.
--
-- This is distinct from DVFS ramp (gpu_dvfs_ramp.sql). A ramp is the transient
-- low-clock period right after waking from idle. Throttling is the clock falling
-- back below target *after* it had reached target within the same burst -- i.e.
-- the governor pulling the clock down while there is still work to do. That
-- usually means a physical limit: thermal or power.
--
-- An interval counts as throttling when, while the GPU is busy:
--   1. freq < target (90% of the per-GPU max OBSERVED frequency), AND
--   2. it starts at/after the moment the clock first reached target following
--      the most recent idle->busy edge (so the initial ramp is excluded), AND
--   3. it lasts at least min_sustained_ns.
-- To change the thresholds, edit the 0.9 factor and the `>= 1000` floor below.
-- The 1000 ns floor only drops sub-microsecond noise; raise it (e.g. to 1e6 for
-- 1 ms) to focus on genuinely sustained throttling.
--
-- Temperature (C) and Power (W) are read from the canonical per-GPU counter
-- tracks if present and averaged over each throttle interval; NULL means no
-- such track, or no sample landed inside a short interval. High temp + capped
-- clock => thermal; high power + capped clock => power-limited.
--
-- Columns (longest throttle intervals first):
--   start_rel_ns : interval start, relative to trace start.
--   dur_ns       : how long the clock was held below target.
--   freq_mhz     : the (reduced) clock during the interval.
--   target_mhz   : 90% of fmax.
--   temp_c       : mean GPU temperature over the interval (NULL if unavailable).
--   power_w      : mean GPU power over the interval (NULL if unavailable).

INCLUDE PERFETTO MODULE counters.intervals;

INCLUDE PERFETTO MODULE intervals.overlap;

INCLUDE PERFETTO MODULE intervals.intersect;

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

-- For each busy edge, the instant the clock first reached target after it.
CREATE PERFETTO TABLE _edges AS
SELECT
  b.ugpu,
  b.ts AS edge_ts,
  (
    SELECT MIN(f.ts)
    FROM _gpu_freq AS f
    WHERE
      f.ugpu = b.ugpu
      AND f.freq_khz >= 0.9 * (SELECT fmax_khz FROM _fmax WHERE ugpu = b.ugpu)
      AND f.ts >= b.ts
      AND f.ts < b.te
  ) AS ramp_reach_ts
FROM _gpu_busy AS b;

-- Busy time spent below target (busy intersect low-frequency intervals).
CREATE PERFETTO TABLE _low_busy AS
SELECT ii.ts, ii.dur, f.freq_khz, b.ugpu
FROM _interval_intersect!((_gpu_busy, _gpu_freq), (ugpu)) AS ii
JOIN _gpu_busy AS b
  ON b.id = ii.id_0
JOIN _gpu_freq AS f
  ON f.id = ii.id_1
WHERE
  f.freq_khz < 0.9 * (SELECT fmax_khz FROM _fmax WHERE ugpu = b.ugpu);

SELECT
  l.ugpu AS gpu,
  IFNULL(g.name, 'GPU ' || l.ugpu) AS gpu_name,
  l.ts - trace_start() AS start_rel_ns,
  l.dur AS dur_ns,
  l.freq_khz / 1000 AS freq_mhz,
  CAST(0.9 * (SELECT fmax_khz FROM _fmax WHERE ugpu = l.ugpu) AS INT) / 1000 AS target_mhz,
  (
    SELECT ROUND(AVG(c.value), 1)
    FROM counter AS c
    JOIN gpu_counter_track AS t ON t.id = c.track_id
    WHERE
      t.name = 'Temperature'
      AND t.ugpu = l.ugpu
      AND c.ts >= l.ts
      AND c.ts < l.ts + l.dur
  ) AS temp_c,
  (
    SELECT ROUND(AVG(c.value), 1)
    FROM counter AS c
    JOIN gpu_counter_track AS t ON t.id = c.track_id
    WHERE
      t.name = 'Power'
      AND t.ugpu = l.ugpu
      AND c.ts >= l.ts
      AND c.ts < l.ts + l.dur
  ) AS power_w
FROM _low_busy AS l
LEFT JOIN gpu AS g ON g.ugpu = l.ugpu
WHERE
  l.dur >= 1000
  AND l.ts
  >= (
    SELECT MAX(e.ramp_reach_ts)
    FROM _edges AS e
    WHERE
      e.ugpu = l.ugpu
      AND e.edge_ts <= l.ts
      AND e.ramp_reach_ts IS NOT NULL
  )
ORDER BY
  l.dur DESC
LIMIT 20;
