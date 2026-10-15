---
name: "Java heap dump before/after diff: leaked session state after 50 iterations"
tags: [android, memory, heap-dump, diff, workflow]
runs: 3
files:
  - src: "{repo}/test/data/android_memory/perfetto_java_heap_dump_before.perfetto-trace"
    dst: heap_before.perfetto-trace
  - src: "{repo}/test/data/android_memory/perfetto_java_heap_dump_after.perfetto-trace"
    dst: heap_after.perfetto-trace
---
We captured two Perfetto Java heap dump traces for `com.android.systemui.qs.panel`
before and after 50 iterations of a UI action: `./heap_before.perfetto-trace`
and `./heap_after.perfetto-trace`. Which Java classes leaked instances between
the two heap dumps, how much retained (dominated) memory did they add, and what
is the exact dominator retention path keeping them alive?
