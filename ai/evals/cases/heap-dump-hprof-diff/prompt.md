---
name: "Java HPROF heap dump field-value leak attribution"
tags: [android, memory, hprof, heap-dump, diff, workflow]
runs: 3
files:
  - src: "{repo}/test/data/android_memory/java_heap_dump_before.hprof"
    dst: before.hprof
  - src: "{repo}/test/data/android_memory/java_heap_dump_after.hprof"
    dst: after.hprof
---
Analyze the before and after Android Java HPROF heap dumps captured after 50
iterations of an action in `com.android.systemui.qs.panel`: `./before.hprof` and
`./after.hprof`. Both dumps retain `com.android.systemui.qs.panel.TileSessionState`
instances in a shared collection. Inspect the String and primitive field values
of those instances to determine which specific tile specification (`tileSpec`)
and session state leaked.
