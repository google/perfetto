---
name: GPU-bound or host-bound
tags: [gpu, workflow, mentions-perfetto]
runs: 3
files:
  - src: "{repo}/test/data/gpu_render_stages.pftrace"
    dst: game.pftrace
---
Our rendering benchmark is slower than we expect. I captured a Perfetto
trace with GPU render stages at ./game.pftrace. Is the workload GPU-bound
or host-bound? Back the verdict with numbers from the trace.
