# GPU compute kernels — which kernel is the bottleneck, and why?

This workflow decomposes a trace's GPU **compute** work (CUDA / ROCm / compute
dispatches) kernel by kernel: which kernel dominates, whether it is
compute-bound, memory-bound, or latency/occupancy-bound, and which specific
resource — a pipeline, the occupancy limit, a cache level, the launch config —
is the lever. A single device-wide "GPU utilization %" cannot answer any of
these; you need the per-kernel decomposition.

It decomposes each compute kernel into four lenses — Speed of Light, Occupancy,
Workload Analysis, and Launch Statistics — computed directly from the trace's
GPU counters and kernel launch args.

If the user has not yet loaded a trace into `trace_processor`, follow
`../../../infra-references/querying.md` first, then come back here.

> If `scripts/kernels_summary.sql` returns no rows, the trace has no compute
> dispatches (no GPU slices with `render_stage_category = 2`) and this workflow
> does not apply.

---

## Phase 1: Mandatory first-pass triage — the all-kernels table

Run this before drilling any single kernel; it names the hotspot. The table is
**vendor-neutral** — slice timing and launch shape only. The compute-vs-memory
verdict needs vendor throughput counters and comes from Phase 2 (Speed of Light).

```bash
trace_processor query --query-file scripts/kernels_summary.sql TRACE_FILE
```

Columns: `id` (launch order), `kernel`, `ugpu`, `dur_ns`, `block_size`,
`grid_size`, `registers`.

1.  **Pick the kernel that matters** — usually the one (or the recurring name)
    that dominates `dur_ns`. Note its `id`; every deeper phase reports the same
    `id`, so you can line the kernel up across tables.

2.  **Identify the GPU and vendor.** Read the authoritative vendor and
    architecture for the kernel's `ugpu` from [../gpu_info.md](../gpu_info.md);
    that decides which vendor deep-dive to use, and enumerates every GPU and
    machine so you scope correctly (per `ugpu`). The metrics below are described
    by their vendor-neutral display names; each section forwards to the matching
    vendor extraction for the concrete counters.

3.  **Classify the kernel** in Phase 2 (Speed of Light) — compute-bound,
    memory-bound, or latency/occupancy-bound — then follow the branch it points
    to. (Classification needs vendor throughput counters, so it is not in this
    neutral table.)

---

## Phase 2: Decompose the chosen kernel

Follow the branch(es) your Phase 1 hypothesis points to. Each is its own
workflow; read the one you need.

- **Speed of Light** — compute vs memory vs latency bound, with the cache/DRAM
  breakdown: [speed_of_light.md](speed_of_light.md).
- **Occupancy** — is launch config / resource pressure the limit, and which
  resource: [occupancy.md](occupancy.md).
- **Workload Analysis** — which execution pipeline is saturated (for
  compute-bound kernels): [workload_analysis.md](workload_analysis.md).
- **Launch Statistics** — the raw launch configuration and whether it is sane:
  [launch_statistics.md](launch_statistics.md).

Each section workflow is vendor-neutral in its interpretation and forwards to a
vendor-specific extraction (e.g. `nvidia/…`) for the actual counters.

---

## Phase 3: Reporting

A good summary states:

1.  **Which kernel** — the hotspot (`id`, name, `dur_ns`, and its share of GPU
    time if known).
2.  **The bound type** — compute / memory / latency-occupancy bound, with the
    numbers (Compute Throughput, Memory Throughput, Achieved Occupancy, the
    saturated pipe).
3.  **The lever** — named concretely: a launch-config change (block size,
    registers, shared memory) to lift occupancy; a precision / instruction-mix
    change for a saturated pipe; a memory-layout / locality change for a
    memory-bound kernel.

To compare two kernels (e.g. before/after, or two variants), run the same
Phase-2 scripts and diff the rows for the two `id`s — every script emits one row
per kernel, so a baseline comparison is a row-vs-row read.

Keep the SQL and kernel ids available so the user can audit.

## Reference

- GPU data sources: <https://perfetto.dev/docs/data-sources/gpu>
- Compute kernel = `gpu_slice.render_stage_category = 2` (the COMPUTE
  render-stage category); compute counters = `gpu_counter_group.group_id = 6`
  (the `GpuCounterGroup` COMPUTE value), matched per `ugpu` over the kernel
  window. (The two "compute" magic numbers, 2 and 6, are different enums.)
