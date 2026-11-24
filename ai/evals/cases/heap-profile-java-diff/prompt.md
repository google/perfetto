---
name: "Java heap allocation profile diff: allocation churn hotspot"
tags: [android, memory, heap-profile, java, diff, workflow]
runs: 3
files:
  - src: "{repo}/test/data/android_memory/java_alloc_profile_before.perfetto-trace"
    dst: java_alloc_before.perfetto-trace
  - src: "{repo}/test/data/android_memory/java_alloc_profile_after.perfetto-trace"
    dst: java_alloc_after.perfetto-trace
---
Compare the before and after Perfetto Java heap allocation profile traces
captured during a UI action in `com.android.systemui.qs.panel`:
`./java_alloc_before.perfetto-trace` and `./java_alloc_after.perfetto-trace`.
How much did Java heap (`com.android.art`) allocation churn increase in the
after trace, and which Java method is the hotspot responsible for the
regression?
