---
type: llm
criteria: |
  The answer concludes that the workload is GPU-bound (the GPU is busy
  nearly all of the time it is active, ~85-100% busy depending on which
  render-stage tracks are counted, ~116k activities across ~8.8 s), and
  it supports that with a busy/idle percentage or duration computed from
  the trace. An answer that says "host-bound" or that gives no numeric
  utilisation evidence fails.
---
Ground truth from the skill's gpu_timeline_decomposition.sql: gpu_busy
7.67 s of 7.69 s active span (99.8%), 87.3% of the 8.79 s trace.
