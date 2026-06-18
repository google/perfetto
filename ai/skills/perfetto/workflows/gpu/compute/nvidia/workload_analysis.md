# NVIDIA — workload analysis extraction

The NVIDIA extraction behind the saturated-pipe / instruction-mix analysis. Run
it, then apply the interpretation from
[../workload_analysis.md](../workload_analysis.md).

```bash
trace_processor query --query-file scripts/workload_analysis.sql TRACE_FILE
```

One row per compute kernel (longest first). Display label → output column →
NVIDIA counter:

| Display label (generic) | Column | NVIDIA counter |
|---|---|---|
| Compute-unit busy | `sm_busy_pct` | `sm__instruction_throughput.avg.pct_of_peak_sustained_active` |
| Executed IPC | `ipc_active` | `sm__inst_executed.avg.per_cycle_active` |
| Pipeline utilization | `alu_pct`, `fma_pct`, `fp16_pct`, `fp32_pct`, `fp64_pct`, `tensor_pct` | `sm__pipe_<x>_cycles_active.avg.pct_of_peak_sustained_active` |

On NVIDIA the compute unit is the **SM**; the per-pipe columns are the SM
execution units. The highest pipe is the bottleneck candidate, and the lever
depends on which pipe it is:

- **`tensor_pct`** high → already on the tensor-core / matrix path (good for
  matmul-like work); gains need larger tiles or a different algorithm, not a mix
  change.
- **`fma_pct` / `fp32_pct`** high → FP32 math bound; consider lower precision
  (fp16 / bf16) if the algorithm tolerates it. (bf16 work shows up under the
  fp16 / tensor pipes — there is no dedicated bf16 cycles-active counter in this
  set.)
- **`fp64_pct`** high → double precision is expensive; drop to fp32 where
  accuracy allows.
- **`alu_pct`** high → integer / address math bound; reduce index arithmetic, or
  precompute.

These are the pipe **cycles-active** counters (the saturation signal); the
paired `sm__inst_executed_pipe_*` (instructions-executed-per-pipe) counters are
not extracted here.

<!-- Architecture-specific guidance (e.g. nvidia/h100/) would forward from here:
     per-arch peak issue rate, tensor-core generation and supported precisions. -->
