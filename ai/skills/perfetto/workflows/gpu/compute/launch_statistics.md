# Compute kernel — launch statistics

The launch configuration defines the size of the work, how it divides into
blocks, and the per-block GPU resources. An inefficient config caps device
utilization before a single instruction runs. This data is **vendor-neutral** —
it comes from launch args the trace records the same way for every compute
producer — so this workflow is self-contained (no vendor layer).

> **Naming note — the one exception.** The launch-arg names below
> (`workgroup_size`, `grid_size`, `registers_per_thread`, `occupancy_limit_*`, …)
> are CUDA-derived, but the trace records them under these same keys regardless
> of GPU vendor. This is the **only** generic compute skill that uses
> vendor-origin names; every other generic skill uses vendor-neutral display
> names. The concepts are universal (block ≈ workgroup, grid ≈ NDRange, compute
> unit ≈ SM / CU).

## Get the data

```bash
trace_processor query --query-file scripts/launch_statistics.sql TRACE_FILE
```

Columns: `id`, `kernel`, `block_size`, `grid_size`, `thread_count`,
`registers`, `shared_mem_static`, `shared_mem_dynamic`, `shared_mem_config`,
`barriers_per_block`, `waves_per_sm`, `func_cache_config`.

## Interpret

- **`block_size`** should be a multiple of the warp/wavefront size (**32 on
  NVIDIA, 64 on AMD**). A non-multiple wastes part of every warp/wavefront. Very
  small blocks underuse each compute unit; very large blocks raise
  register/shared-memory pressure.
- **`grid_size` / `waves_per_sm`** gauge whether the launch fills the GPU:
  `waves_per_sm < 1` means the grid is too small (idle compute units); `1–2`
  risks tail effects; `≥ 4` balances load. (Occupancy analysis goes deeper —
  [occupancy.md](occupancy.md).)
- **`registers` and `shared_mem_*`** are the resources that most often cap
  occupancy; high values for either limit how many blocks fit per compute unit.
  Cross-check against the binding resource in [occupancy.md](occupancy.md).
- **`func_cache_config`** (e.g. prefer-L1 vs prefer-shared) should match the
  kernel's actual shared-memory use; a mismatch wastes the carveout.

Launch Statistics is descriptive: it tells you what was requested. Whether the
config actually limited the kernel is answered by [occupancy.md](occupancy.md)
(resource limits) and [speed_of_light.md](speed_of_light.md) (bound type).
