# Compute kernel — occupancy and its limiting factor

Occupancy is the ratio of active warps/wavefronts per compute unit to the
hardware maximum. Higher occupancy helps hide memory and instruction latency,
but more is not always better — once latency is hidden, extra occupancy does
nothing, and chasing it can cost registers or shared memory. This workflow asks
two things: is *this* kernel limited by occupancy, and if so, which resource
caps it. This is the interpretation layer; run the vendor extraction and read
the values back here.

## Get the data

- **NVIDIA / CUDA:** [nvidia/occupancy.md](nvidia/occupancy.md).
- Other vendors: the per-resource block limits are launch args (often present
  regardless of vendor); achieved occupancy needs a vendor counter.

## The metrics

By their display names (the vendor extraction maps these to its counters/args):

- **Theoretical Occupancy** — the ceiling the launch config allows (% of max).
- **Achieved Occupancy** — what actually ran (% of max).
- **Block Limit (registers / shared memory / warps / blocks / barriers)** — how
  many blocks fit per compute unit under each resource; the smallest is the
  binding constraint.
- **Block Size**, **Grid Size**, **Registers**, **Shared Memory**, **Waves per
  compute unit** — the launch shape.

## Interpret

- **Block size rule.** Block size should be a multiple of the warp/wavefront
  size — **32 on NVIDIA, 64 on AMD**. A non-multiple wastes a fraction of every
  warp/wavefront. Very small blocks underuse each compute unit; very large
  blocks raise register/shared-memory pressure and can cap occupancy.
- **Waves rule.** Waves per compute unit (grid size / resident blocks per unit):
  - **< 1** → the grid is too small to fill the GPU; units sit idle. Lever: more
    parallelism (larger grid) or fuse with other work.
  - **1–2** → tail effects: some units finish early and idle while the last wave
    drains. Lever: size the grid to a larger whole number of waves.
  - **≥ 4** → good load balance.
- **Theoretical vs Achieved gap.** A large gap (Achieved ≪ Theoretical) means
  the kernel isn't sustaining the warps/wavefronts the config allows — imbalance,
  tail effects, or early exits. A small gap with low Theoretical means the launch
  config itself is the cap → look at the binding Block Limit.
- **Binding resource.** The Block Limit with the fewest blocks per compute unit
  is the thing to relax to raise Theoretical Occupancy:
  - **registers** → reduce register pressure (smaller types, fewer live values,
    or a launch-bounds hint).
  - **shared memory** → use less per block, or a smaller carveout.
  - **warps / blocks** → block size or the per-unit hardware limit; resize blocks.
  - **barriers** → fewer named barriers per block.

Only chase occupancy if the kernel is actually latency-bound (Speed of Light
showed both ceilings low). A compute- or memory-bound kernel at modest occupancy
is fine — raising occupancy won't help.
