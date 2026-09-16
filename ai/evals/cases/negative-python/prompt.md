---
name: Negative control, plain Python profiling
tags: [negative, trigger]
runs: 3
files:
  - src: slow.py
    dst: slow.py
---
./slow.py takes longer than I'd like to run. Profile it and tell me which
function dominates the runtime.
