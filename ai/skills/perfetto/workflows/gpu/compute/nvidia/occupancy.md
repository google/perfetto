# NVIDIA — occupancy extraction

The NVIDIA extraction behind the occupancy analysis. Run it, then apply the
block-size / waves / binding-resource interpretation from
[../occupancy.md](../occupancy.md).

```bash
trace_processor query --query-file scripts/occupancy.sql TRACE_FILE
```

One row per compute kernel (longest first). Display label → output column →
NVIDIA counter / launch arg:

| Display label (generic) | Column | NVIDIA counter / arg |
|---|---|---|
| Theoretical Occupancy | `theoretical_occ_pct` | `sm__maximum_warps_per_active_cycle_pct` (arg) |
| Achieved Occupancy | `achieved_occ_pct` | `sm__warps_active.avg.pct_of_peak_sustained_active` (counter) |
| Block Limit (blocks) | `limit_blocks` | `occupancy_limit_blocks` (arg) |
| Block Limit (registers) | `limit_registers` | `occupancy_limit_registers` (arg) |
| Block Limit (shared memory) | `limit_shared_mem` | `occupancy_limit_shared_mem` (arg) |
| Block Limit (warps) | `limit_warps` | `occupancy_limit_warps` (arg) |
| Block Limit (barriers) | `limit_barriers` | `occupancy_limit_barriers` (arg) |

Also emitted (launch shape): `block_size`, `grid_size`, `registers`,
`shared_mem_static`, `shared_mem_dynamic`, `waves_per_sm`, and
`binding_resource` (the resource at the smallest block limit).

On NVIDIA the compute unit is the **SM** and the warp size is **32**, so
`block_size` should be a multiple of 32. `binding_resource` is the
`occupancy_limit_*` with the fewest blocks per SM — the resource to relax.

<!-- Architecture-specific guidance (e.g. nvidia/h100/) would forward from here:
     SM count, max warps/registers/shared-mem per SM for the exact arch. -->
