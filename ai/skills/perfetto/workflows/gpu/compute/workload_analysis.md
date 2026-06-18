# Compute kernel — workload analysis (which pipeline is the limit?)

For a compute-bound kernel, "compute-bound" isn't the end of the story — *which*
execution pipeline is saturated determines the lever. This workflow looks at
instruction-issue throughput and per-pipeline utilization to find the saturated
pipe and spot instruction-mix imbalance. Run it when Speed of Light showed the
kernel compute-bound. This is the interpretation layer; run the vendor
extraction and read the values back here.

## Get the data

- **NVIDIA / CUDA:** [nvidia/workload_analysis.md](nvidia/workload_analysis.md).
- Other vendors: the per-pipeline breakdown is vendor-specific; for others use
  whatever instruction-throughput / IPC that vendor exposes.

## The metrics

By their display names (the vendor extraction maps these to its counters and
lists the concrete pipelines that vendor exposes):

- **Compute-unit busy** — instruction-issue throughput, % of peak.
- **Executed IPC** — instructions executed per active cycle.
- **Per-pipeline utilization** — % of peak for each execution pipeline. *The set
  of pipelines is vendor-specific* — see the vendor extraction for which pipes
  exist and what each maps to.

## Interpret

- **One pipe near peak, others low → that pipe is the bottleneck.** The lever is
  to move work off it: a cheaper precision/instruction mix if the work can use
  one, or an algorithm that needs less of that pipe. Which precision is cheaper,
  and which pipe carries it, is vendor-specific — see the vendor extraction.
- **Compute-unit busy high but no single pipe near peak → issue-bound / mixed.**
  The unit is busy issuing but work is spread across pipes; the limit is
  instruction issue itself. Lever: fewer instructions (algorithmic), or better
  ILP.
- **Compute-unit busy low and all pipes low → not compute-bound after all.** The
  kernel is stalling — it is memory- or latency-bound; the lever is in the Speed
  of Light / occupancy view, not here.
- **Imbalance across precision pipes** (two precision paths both moderate) can
  mean the kernel mixes precisions; consolidating onto the cheapest sufficient
  precision frees issue slots.
