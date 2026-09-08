---
name: Top CPU thread in an Android system trace
tags: [android, cpu, adhoc, mentions-perfetto]
runs: 3
files:
  - src: "{repo}/test/data/example_android_trace_30s.pb"
    dst: trace.pb
---
I have a Perfetto trace from an Android phone at ./trace.pb. Which thread
used the most CPU time over the whole trace, and roughly how much? Give me
the thread name and the process it belongs to.
