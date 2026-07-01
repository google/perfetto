# NVIDIA — Speed of Light extraction

The NVIDIA counter extraction behind the compute-vs-memory-vs-latency verdict.
Run it, then apply the bound-type interpretation from
[speed_of_light.md]($SKILL_ROOT/workflows/gpu/compute/speed_of_light.md).

```bash
trace_processor query --query-file $SKILL_ROOT/workflows/gpu/compute/nvidia/scripts/speed_of_light.sql TRACE_FILE
```

One row per compute kernel (longest first). Display label → output column →
NVIDIA counter:

| Display label (generic) | Column | NVIDIA counter |
|---|---|---|
| Compute Throughput | `compute_pct` | `sm__throughput.avg.pct_of_peak_sustained_elapsed` |
| Memory Throughput | `memory_pct` | `gpu__compute_memory_throughput.avg.pct_of_peak_sustained_elapsed` |
| L1 Cache Throughput | `l1_pct` | `l1tex__throughput.avg.pct_of_peak_sustained_active` |
| L2 Cache Throughput | `l2_pct` | `lts__throughput.avg.pct_of_peak_sustained_elapsed` |
| DRAM Throughput | `dram_pct` | `gpu__dram_throughput.avg.pct_of_peak_sustained_elapsed` |
| Elapsed Cycles | `elapsed_cycles` | `gpc__cycles_elapsed.max` |
| Active Cycles | `active_cycles` | `sm__cycles_active.avg` |
| Duration | `dur_ns` | `gpu__time_duration.sum` |

On NVIDIA the compute unit is the **SM**. The throughput columns are *% of peak*
(how saturated the level was), **not** cache hit rates: a high `dram_pct` is a
true DRAM-bandwidth bound, while high `l1_pct`/`l2_pct` with low `dram_pct` means
the working set is served from cache rather than DRAM.

<!-- Architecture-specific peak/lever guidance (e.g. nvidia/h100/) would forward
     from here once added: compare achieved % against the arch's absolute peaks
     (HBM bandwidth, tensor TFLOPs) and arch-specific levers (FP8, TMA). -->
