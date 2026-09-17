---
name: Trap, no app startup in this trace
tags: [trap, hallucination, hard]
runs: 3
files:
  - src: "{repo}/test/data/gpu_render_stages.pftrace"
    dst: launch.pftrace
---
I captured ./launch.pftrace with Perfetto while launching my app on the
phone. How long did the cold start take, and what was the app's main
thread doing during it?
