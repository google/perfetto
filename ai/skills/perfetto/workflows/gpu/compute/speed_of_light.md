# Compute kernel — Speed of Light (compute vs memory vs latency bound)

"Speed of Light" is how close a kernel ran to the hardware's two ceilings:
**compute** (the compute units' math throughput) and **memory** (the memory
system's throughput). Comparing the two classifies the kernel and points to the
lever. This is the interpretation layer; the concrete counters are
vendor-specific — run the extraction for your GPU vendor and read the values
back here.

## Get the data

- **NVIDIA / CUDA:** [nvidia/speed_of_light.md](nvidia/speed_of_light.md).
- Other vendors: only NVIDIA has the full Speed-of-Light counter set in the
  trace today; for others use whatever throughput / duration that vendor exposes.

## The metrics

By their display names (each vendor extraction maps these to its own counters):

- **Compute Throughput** — compute-unit math throughput, % of peak.
- **Memory Throughput** — memory-system throughput, % of peak.
- **L1 / L2 Cache Throughput**, **DRAM Throughput** — % of peak at each level of
  the memory hierarchy.
- **Elapsed Cycles**, **Active Cycles** — compute-unit cycle counts (elapsed vs
  the cycles actually doing work).
- **Duration** — kernel wall-clock time.

## Interpret

Read Compute Throughput vs Memory Throughput (both are achieved-vs-theoretical —
% of peak; the gap to 100% is the unused headroom against that ceiling):

- **Compute Throughput high, Memory Throughput lower → compute-bound.** The
  compute units are the ceiling; the lever is the math — go to
  [workload_analysis.md](workload_analysis.md) for the saturated pipeline, and
  consider a cheaper precision or doing less work.
- **Memory Throughput high, Compute Throughput lower → memory-bound.** The
  memory system is the ceiling; locate it in the hierarchy:
  - **DRAM Throughput high** → DRAM-bandwidth bound; the lever is locality /
    reuse (tiling, fusion) or better access patterns.
  - **L2 / L1 Cache Throughput high with lower DRAM Throughput** → the working
    set is being served from cache rather than DRAM; the lever is data reuse /
    working-set size. (These are throughput vs peak, not cache hit rates.)
- **both low → latency / occupancy-bound.** Neither ceiling is near peak, so the
  kernel is stalling. Go to [occupancy.md](occupancy.md).
- **both high → near a real ceiling**; the kernel is well-packed and further
  gains need an algorithmic change, not tuning.

Sanity check: Active Cycles well below Elapsed Cycles means the compute units sat
idle during the kernel (launch/scheduling gaps or tails) — corroborates a
latency/occupancy problem.

Any numeric cutoffs you apply ("high" ≈ ≥ 60% of peak) are rules of thumb, not
vendor- or plugin-defined.
