# GPU frequency residency — the GPU is busy, but at what clock?

This workflow looks at the GPU's clock while it is doing work. Whether the GPU
is **busy** or **idle** is a separate question; this one assumes you already
have busy time to explain and asks, *during that busy time*, whether the device
ran at its peak frequency or was held back. A GPU that is busy 100% of the time
but pinned at half its clock is leaving half its throughput on the table — and a
busy/idle view, or a steady-state frequency average, cannot see that.

It works on any trace that has a canonical GPU **frequency** track — the
`gpufreq` counter track (one per GPU). It catches the two classic,
workload-agnostic clock pathologies:

- **Slow DVFS ramp** after an idle→busy transition: the clock starts at a floor
  state and takes time to ramp up, so the work that woke the GPU runs slow.
  Bursty workloads (UI rendering, inference serving) pay this repeatedly and it
  is invisible in pure occupancy.
- **Sustained throttling**: the clock held below peak *while busy*, usually
  because of a physical limit — correlated here against temperature and power.

If the user has not yet loaded a trace into `trace_processor`, follow
`../../infra-references/querying.md` first, then come back here.

> If `gpu_frequency_residency.sql` returns no rows, the trace has no `gpufreq`
> track and this workflow does not apply.

---

## Phase 1: Mandatory first-pass triage

Run this before any open-ended exploration. It produces the headline verdict.

1.  Run the residency script. It takes the trace file as its argument and
    returns one row per GPU (that has a frequency track) as CSV.

    ```bash
    trace_processor query --query-file scripts/gpu_frequency_residency.sql TRACE_FILE
    ```

    Columns: `gpu`, `gpu_name`, `active_span_ns`, `gpu_busy_ns`,
    `busy_pct_of_active`, `fmax_mhz`, `mean_busy_mhz`, `eff_occupancy_pct`,
    `freq_coverage_pct`.

2.  Interpret. The headline is `eff_occupancy_pct` vs `busy_pct_of_active`:

    - **`eff_occupancy_pct` ≈ `busy_pct_of_active`** — when the GPU is busy it
      runs at (near) peak clock. The clock is healthy; the lever is elsewhere —
      the device work itself, or the idle between work, not the clock. When that
      device work is GPU **compute** kernels, decompose it — which kernel, and
      what bounds it — with
      [compute/kernel_analysis.md](compute/kernel_analysis.md); otherwise stop here.
    - **`eff_occupancy_pct` ≪ `busy_pct_of_active`** — the GPU is busy but
      underclocked. The clock *is* the problem. Proceed to Phase 2 (ramp) and
      Phase 3 (throttling) to find out why.
    - **`fmax_mhz` is the max OBSERVED frequency**, not necessarily the hardware
      ceiling. If the workload never demanded peak, `fmax` understates the true
      maximum and `eff_occupancy_pct` is optimistic. Confirm the expected peak
      with the user if it matters.
    - **`freq_coverage_pct` well below 100** — some busy time had no frequency
      sample; the figures are partial. Say so.

3.  For the breakdown behind `mean_busy_mhz`, list the time spent at each clock
    while busy (the residency histogram):

    ```sql
    INCLUDE PERFETTO MODULE counters.intervals;
    INCLUDE PERFETTO MODULE intervals.overlap;
    INCLUDE PERFETTO MODULE intervals.intersect;

    CREATE PERFETTO TABLE _gpu_freq AS
      SELECT f.id, f.ts, f.dur, f.value AS freq_khz, gct.ugpu
      FROM counter_leading_intervals!((
        SELECT c.id, c.ts, c.track_id, c.value
        FROM counter AS c
        JOIN gpu_counter_track AS gct ON gct.id = c.track_id
        WHERE gct.name = 'gpufreq')) AS f
      JOIN gpu_counter_track AS gct ON gct.id = f.track_id;

    CREATE PERFETTO TABLE _gpu_busy AS
      SELECT ROW_NUMBER() OVER (ORDER BY ugpu, ts) AS id, ugpu, ts, dur
      FROM interval_merge_overlapping_partitioned!((
        SELECT s.ts, s.dur,
               IFNULL(EXTRACT_ARG(t.dimension_arg_set_id, 'ugpu'), 0) AS ugpu
        FROM gpu_slice AS s JOIN gpu_track AS t ON s.track_id = t.id
        WHERE s.dur > 0), (ugpu));

    SELECT b.ugpu AS gpu, f.freq_khz / 1000 AS freq_mhz,
           SUM(ii.dur) AS busy_at_freq_ns,
           ROUND(100.0 * SUM(ii.dur)
                 / SUM(SUM(ii.dur)) OVER (PARTITION BY b.ugpu), 1) AS pct_of_busy
    FROM _interval_intersect!((_gpu_busy, _gpu_freq), (ugpu)) AS ii
    JOIN _gpu_busy AS b ON b.id = ii.id_0
    JOIN _gpu_freq AS f ON f.id = ii.id_1
    GROUP BY b.ugpu, f.freq_khz
    ORDER BY b.ugpu, f.freq_khz;
    ```

4.  If the workload repeats a phase (e.g. an iteration marked by a named slice),
    compute `eff_occupancy_pct` per window — usually the number that matters.
    Substitute the boundary slice name; append this after the `_gpu_freq` /
    `_gpu_busy` tables from step 3:

    ```sql
    CREATE PERFETTO TABLE windows AS
      SELECT ROW_NUMBER() OVER (ORDER BY ts) AS id, name, ts, dur
      FROM slice WHERE name GLOB 'YOUR_BOUNDARY_SLICE*' AND dur > 0;

    CREATE PERFETTO TABLE _baf_iv AS
      SELECT ROW_NUMBER() OVER (ORDER BY ii.ts) AS id, ii.ts, ii.dur,
             f.freq_khz, b.ugpu
      FROM _interval_intersect!((_gpu_busy, _gpu_freq), (ugpu)) AS ii
      JOIN _gpu_busy AS b ON b.id = ii.id_0
      JOIN _gpu_freq AS f ON f.id = ii.id_1;

    SELECT w.name AS window, b.ugpu AS gpu, w.dur AS window_ns,
           ROUND(100.0 * SUM(b.freq_khz * ii.dur)
                 / (w.dur * (SELECT MAX(freq_khz) FROM _gpu_freq
                             WHERE ugpu = b.ugpu)), 1) AS eff_occupancy_pct
    FROM _interval_intersect!((windows, _baf_iv), ()) AS ii
    JOIN windows AS w ON w.id = ii.id_0
    JOIN _baf_iv AS b ON b.id = ii.id_1
    GROUP BY w.id, w.name, b.ugpu, w.dur
    ORDER BY w.ts;
    ```

---

## Phase 2: Slow DVFS ramp

> Run this when Phase 1 shows the GPU is busy but underclocked.

1.  List the ramp events — idle→busy edges where the clock had to climb to reach
    a healthy state:

    ```bash
    trace_processor query --query-file scripts/gpu_dvfs_ramp.sql TRACE_FILE
    ```

    Returns, worst first: `gpu`, `gpu_name`, `edge_rel_ns`, `idle_gap_ns`,
    `freq_at_edge_mhz`, `target_mhz` (90% of `fmax`), `ramp_ns`, `completed`.
    `idle_gap_ns` is NULL for the first burst (cold start). `completed = 0`
    means the burst ended while still ramping, so `ramp_ns` is a lower bound.

2.  Interpret:

    - **Many edges with large `ramp_ns`** — the workload keeps letting the GPU
      go idle and pays the wake-up cost each time. The lever is upstream: shrink
      the idle gaps (batch work to amortize wake-ups, keep the device warm, or
      tune the governor). Note that each such idle gap costs twice: the idle
      itself, plus this ramp tax once work resumes.
    - **`completed = 0` ramps** — bursts so short the clock never even reaches
      peak before the work is done; the workload is paying ramp cost for almost
      no peak-clock time.

3.  For the aggregate tax — how much wall-clock is spent ramping — run the
    summary:

    ```sql
    INCLUDE PERFETTO MODULE counters.intervals;
    INCLUDE PERFETTO MODULE intervals.overlap;

    CREATE PERFETTO TABLE _gpu_freq AS
      SELECT f.id, f.ts, f.dur, f.value AS freq_khz, gct.ugpu
      FROM counter_leading_intervals!((
        SELECT c.id, c.ts, c.track_id, c.value
        FROM counter AS c
        JOIN gpu_counter_track AS gct ON gct.id = c.track_id
        WHERE gct.name = 'gpufreq')) AS f
      JOIN gpu_counter_track AS gct ON gct.id = f.track_id;

    CREATE PERFETTO TABLE _gpu_busy AS
      SELECT ugpu, ts, ts + dur AS te
      FROM interval_merge_overlapping_partitioned!((
        SELECT s.ts, s.dur,
               IFNULL(EXTRACT_ARG(t.dimension_arg_set_id, 'ugpu'), 0) AS ugpu
        FROM gpu_slice AS s JOIN gpu_track AS t ON s.track_id = t.id
        WHERE s.dur > 0), (ugpu));

    CREATE PERFETTO TABLE _fmax AS
      SELECT ugpu, MAX(freq_khz) AS fmax_khz FROM _gpu_freq GROUP BY ugpu;

    CREATE PERFETTO TABLE _ramps AS
      SELECT b.ugpu,
        IFNULL((SELECT MIN(f.ts) FROM _gpu_freq AS f
                WHERE f.ugpu = b.ugpu
                  AND f.freq_khz >= 0.9 * (SELECT fmax_khz FROM _fmax WHERE ugpu = b.ugpu)
                  AND f.ts >= b.ts AND f.ts < b.te), b.te) - b.ts AS ramp_ns
      FROM _gpu_busy AS b
      WHERE (SELECT f.freq_khz FROM _gpu_freq AS f
             WHERE f.ugpu = b.ugpu AND f.ts <= b.ts AND f.ts + f.dur > b.ts)
            < 0.9 * (SELECT fmax_khz FROM _fmax WHERE ugpu = b.ugpu);

    SELECT ugpu AS gpu, COUNT(*) AS n_ramps, SUM(ramp_ns) AS total_ramp_ns,
           CAST(AVG(ramp_ns) AS INT) AS mean_ramp_ns, MAX(ramp_ns) AS max_ramp_ns
    FROM _ramps GROUP BY ugpu;
    ```

---

## Phase 3: Sustained throttling

> Run this when Phase 1 shows underclocking that Phase 2 does not fully explain
> (i.e. the clock is low even outside the post-wake ramp).

1.  List sustained throttle intervals — the clock held below target *after* it
    had already reached target in the same burst — with thermal/power context:

    ```bash
    trace_processor query --query-file scripts/gpu_sustained_throttle.sql TRACE_FILE
    ```

    Returns, longest first: `gpu`, `gpu_name`, `start_rel_ns`, `dur_ns`,
    `freq_mhz`, `target_mhz`, `temp_c`, `power_w`.

2.  Interpret the correlation:

    - **High `temp_c` + capped clock** — thermal throttling. The lever is
      cooling / thermal headroom, not the code.
    - **High `power_w` + capped clock** — power-limited. The lever is the power
      budget / cap.
    - **Neither elevated (or both NULL)** — the cap is policy or governor
      behaviour, not a physical limit; or the trace lacks `Temperature` / `Power`
      tracks for that GPU (NULL). Note which.
    - **No rows** — no sustained throttling; the underclock from Phase 1 is the
      ramp (Phase 2), not throttling.

---

## Phase 4: Reporting

A good summary states:

1.  **The verdict** — clock-healthy, ramp-bound, or throttle-bound, with the
    numbers (`eff_occupancy_pct` vs `busy_pct_of_active`, and per-window if
    available).
2.  **Where the clock loss is** — the worst ramps (with idle gaps) and/or the
    throttle intervals (with the temperature/power that explains them), named
    concretely.
3.  **The lever** — ramp: shrink idle gaps, batch work to amortize wake-ups,
    keep the device warm, tune the governor. Throttle: cooling / power headroom.
    Clock-healthy: the lever is elsewhere — the device work or the idle between
    work, not the clock.

Keep the SQL and timestamps available so the user can audit. For multi-GPU
traces, every script is per-GPU; a GPU with activity but no `gpufreq` track is
simply absent from Phase 1 — say so rather than implying it ran at peak.

## Reference

- GPU data sources: <https://perfetto.dev/docs/data-sources/gpu>
- Canonical frequency track: `gpu_counter_track` where `name = 'gpufreq'`
  (type `gpu_frequency`, kHz); stdlib analog `android.gpu.frequency`.
- Interval helpers: stdlib `counters.intervals`, `intervals.overlap`,
  `intervals.intersect`.
