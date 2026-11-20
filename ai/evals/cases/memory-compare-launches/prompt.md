---
name: Compare an app's memory across two system traces from the same device
tags: [android, memory, diff, workflow]
runs: 3
files:
  - src: "{repo}/test/data/api31_startup_hot.perfetto-trace"
    dst: before.perfetto-trace
  - src: "{repo}/test/data/api31_startup_warm.perfetto-trace"
    dst: after.perfetto-trace
---
I launched `androidx.benchmark.integration.macrobenchmark.target` twice on the
same phone and captured a Perfetto system trace each time:
`./before.perfetto-trace` and `./after.perfetto-trace`. Did the app's memory
grow from the first launch to the second? Compare its Java heap and RSS
between the two traces.
