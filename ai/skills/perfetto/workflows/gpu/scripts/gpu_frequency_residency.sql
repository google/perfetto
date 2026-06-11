-- GPU frequency residency during busy time + effective occupancy.
--
-- Skill "timeline occupancy" answers "is the GPU busy?". This answers the next
-- question: "the GPU is busy, but at what clock?". A device that is busy 100% of
-- the time but pinned at half its peak frequency is leaving half its throughput
-- on the table, and a pure busy/idle decomposition cannot see that.
--
-- It composes with the busy timeline: "busy" is the UNION of GPU activity
-- intervals (concurrency on multiple hardware queues is merged, not summed), and
-- each slice of busy time is attributed to the GPU frequency in effect during
-- it. The frequency source is the canonical per-GPU "gpufreq" counter track
-- (type gpu_frequency, kHz) -- the same track the Perfetto UI renders as the
-- per-GPU "Frequency" track -- expanded into gapless intervals.
--
-- No parameters; operates over each GPU's active span (first..last activity).
-- One row per GPU that has a gpufreq track. Columns:
--   busy_pct_of_active : busy / active span                 -> packing.
--   fmax_mhz           : max OBSERVED frequency (peak seen in this trace, not
--                        necessarily the hardware ceiling).
--   mean_busy_mhz      : time-weighted mean clock while busy.
--   eff_occupancy_pct  : busy_pct_of_active * (mean_busy / fmax). The single
--                        "useful work" figure: how much of peak the GPU
--                        actually delivered over the active span. eff well below
--                        busy_pct_of_active means the clock, not idle, is the
--                        lever (see gpu_dvfs_ramp.sql / gpu_sustained_throttle.sql).
--   freq_coverage_pct  : fraction of busy time that had a frequency sample. If
--                        low, the numbers above are partial -- treat with care.

INCLUDE PERFETTO MODULE counters.intervals;

INCLUDE PERFETTO MODULE intervals.overlap;

INCLUDE PERFETTO MODULE intervals.intersect;

-- Canonical per-GPU frequency timeline (gpufreq), gapless, keyed by ugpu.
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

-- Device-busy timeline per GPU (union of GPU activity).
CREATE PERFETTO TABLE _gpu_busy AS
SELECT ROW_NUMBER() OVER (ORDER BY ugpu, ts) AS id, ugpu, ts, dur
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

-- Busy time attributed to the frequency in effect (busy intersect freq).
CREATE PERFETTO TABLE _busy_at_freq AS
SELECT b.ugpu, f.freq_khz, ii.dur
FROM _interval_intersect!((_gpu_busy, _gpu_freq), (ugpu)) AS ii
JOIN _gpu_busy AS b
  ON b.id = ii.id_0
JOIN _gpu_freq AS f
  ON f.id = ii.id_1;

CREATE PERFETTO TABLE _span AS
SELECT
  ugpu,
  MIN(ts) AS span_start,
  MAX(ts + dur) AS span_end,
  SUM(dur) AS busy_ns
FROM _gpu_busy
GROUP BY
  ugpu;

SELECT
  s.ugpu AS gpu,
  IFNULL(g.name, 'GPU ' || s.ugpu) AS gpu_name,
  s.span_end - s.span_start AS active_span_ns,
  s.busy_ns AS gpu_busy_ns,
  ROUND(100.0 * s.busy_ns / (s.span_end - s.span_start), 1) AS busy_pct_of_active,
  (SELECT MAX(freq_khz) FROM _gpu_freq WHERE ugpu = s.ugpu) / 1000 AS fmax_mhz,
  ROUND(
    (SELECT SUM(dur * freq_khz) FROM _busy_at_freq WHERE ugpu = s.ugpu) * 1.0
    / (SELECT SUM(dur) FROM _busy_at_freq WHERE ugpu = s.ugpu)
    / 1000,
    0
  ) AS mean_busy_mhz,
  ROUND(
    100.0 * (SELECT SUM(dur * freq_khz) FROM _busy_at_freq WHERE ugpu = s.ugpu)
    / ((s.span_end - s.span_start)
    * (SELECT MAX(freq_khz) FROM _gpu_freq WHERE ugpu = s.ugpu)),
    1
  ) AS eff_occupancy_pct,
  ROUND(
    100.0 * (SELECT SUM(dur) FROM _busy_at_freq WHERE ugpu = s.ugpu) / s.busy_ns,
    1
  ) AS freq_coverage_pct
FROM _span AS s
LEFT JOIN gpu AS g ON g.ugpu = s.ugpu
WHERE
  EXISTS (SELECT 1 FROM _gpu_freq WHERE ugpu = s.ugpu)
ORDER BY
  s.ugpu;
