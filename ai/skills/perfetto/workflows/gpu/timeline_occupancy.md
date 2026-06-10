# GPU timeline occupancy — is the GPU actually working?

This workflow decomposes the GPU timeline into **device-busy** vs **idle** time,
so you can tell whether a workload is GPU-bound (optimize the device work) or
host-bound (the device is starved and you must look upstream). It works on any
trace that contains GPU render-stage activity, for any GPU/accelerator vendor.

Why this and not a device "utilization percent" gauge: a coarse utilization
number tells you only that something is *resident* on the device, not how the
wall clock decomposes or where the idle is. The trace-level view — the union of
GPU activity intervals measured against the timeline (or a window of interest) —
is what tells you whether you have a GPU problem or a host problem. The gaps
point you upstream: submission/launch latency, host-side synchronization,
allocation, host-device copies, or host work starving the device.

If the user has not yet loaded a trace into `trace_processor`, follow
`../../infra-references/querying.md` first, then come back here.

---

## Phase 1: Mandatory first-pass triage

Run this before any open-ended exploration. It produces the headline verdict.

1.  Run the decomposition script. It takes the trace file as its argument and
    returns one row per GPU as CSV.

    ```bash
    trace_processor query --query-file scripts/gpu_timeline_decomposition.sql TRACE_FILE
    ```

    Columns: `gpu`, `gpu_name`, `activities`, `trace_wall_ns`, `active_span_ns`,
    `gpu_busy_ns`, `busy_pct_of_active`, `busy_pct_of_trace`.

2.  Interpret:

    - **`busy_pct_of_active` is high (≳ 90%)** — within the window where the GPU
      is doing work, activity is packed back-to-back. This is **GPU-bound**: the
      lever is the device work itself, not the host. Stop here unless the user
      wants deeper device-side analysis.
    - **`busy_pct_of_active` is low** — there are meaningful idle gaps *between*
      activities even while the workload is "running." This is **host-bound** /
      stalled. Proceed to Phase 2 to find and attribute the gaps.
    - **`busy_pct_of_trace` ≪ `busy_pct_of_active`** — the GPU is well-packed
      while active, but a large fraction of wall-clock has no GPU work at all
      (setup, I/O, teardown). Whether that matters depends on the window the
      user cares about; confirm with them.

3.  If the workload repeats a phase (e.g. an iteration marked by a named slice),
    decompose per window — this is usually the number that matters. Pick the
    boundary slice name for the workload and substitute it:

    ```sql
    INCLUDE PERFETTO MODULE intervals.overlap;
    INCLUDE PERFETTO MODULE intervals.intersect;

    CREATE PERFETTO TABLE windows AS
      SELECT ROW_NUMBER() OVER (ORDER BY ts) AS id, name, ts, dur
      FROM slice WHERE name GLOB 'YOUR_BOUNDARY_SLICE*' AND dur > 0;

    CREATE PERFETTO TABLE busy AS
      SELECT ROW_NUMBER() OVER (ORDER BY ts) AS id, ts, dur
      FROM interval_merge_overlapping!((
        SELECT ts, dur FROM gpu_slice WHERE dur > 0), 0);

    SELECT
      w.name AS window,
      w.dur AS window_ns,
      IFNULL(SUM(ii.dur), 0) AS gpu_busy_ns,
      ROUND(100.0 * IFNULL(SUM(ii.dur), 0) / w.dur, 1) AS gpu_busy_pct
    FROM windows w
    LEFT JOIN _interval_intersect!((windows, busy), ()) ii ON ii.id_0 = w.id
    GROUP BY w.id, w.name, w.dur
    ORDER BY w.ts;
    ```

---

## Phase 2: Find and attribute the idle gaps

> Run this when Phase 1 shows the GPU is idle for a window that matters.

1.  List the largest idle gaps on the GPU timeline:

    ```bash
    trace_processor query --query-file scripts/gpu_idle_gaps.sql TRACE_FILE
    ```

    Returns `gap_start_rel_ns` (relative to trace start) and `gap_dur_ns`,
    biggest first.

2.  Attribute the biggest gap to host-side work. Take the gap's absolute window
    `[$gap_start, $gap_end]` (`$gap_start = trace_start() + gap_start_rel_ns`,
    `$gap_end = $gap_start + gap_dur_ns`) and substitute the numeric values —
    `trace_processor` does not interpret `$` variables:

    ```sql
    INCLUDE PERFETTO MODULE slices.with_context;

    SELECT
      s.name,
      s.thread_name,
      COUNT(*) AS n,
      SUM(MIN(s.ts + s.dur, $gap_end) - MAX(s.ts, $gap_start)) AS overlap_ns
    FROM thread_or_process_slice s
    WHERE s.dur > 0 AND s.depth = 0
      AND s.ts < $gap_end AND s.ts + s.dur > $gap_start
    GROUP BY s.name, s.thread_name
    ORDER BY overlap_ns DESC
    LIMIT 10;
    ```

    The top rows name what the host was doing while the GPU sat idle. Common
    causes: device memory allocation/free, host-device copies, host-side
    synchronization or waits, submission/launch overhead dominating, or
    host-side compute (input preparation) starving the device.

3.  For multi-GPU traces, the scripts above merge all GPUs into one timeline.
    To analyze one device, add `AND EXTRACT_ARG(t.dimension_arg_set_id, 'ugpu')
    = N` to the activity selection (joining `gpu_track t`).

---

## Phase 3: Reporting

A good summary states:

1.  **The verdict** — GPU-bound or host-bound, with the numbers
    (`busy_pct_of_active`, and per-window % if available).
2.  **Where the idle is** — the largest gaps and the host work that overlaps
    them, named concretely (slice name, thread).
3.  **The lever** — device-side optimization (GPU-bound) vs. removing the
    upstream stall (host-bound: hide allocation, overlap copies, batch
    submissions, fix the host pipeline).

Keep the SQL and the gap timestamps available so the user can audit.

## Reference

- GPU data sources: <https://perfetto.dev/docs/data-sources/gpu>
- Interval helpers: stdlib `intervals.overlap`, `intervals.intersect`
- PerfettoSQL tour:
  <https://perfetto.dev/docs/analysis/perfetto-sql-getting-started>
