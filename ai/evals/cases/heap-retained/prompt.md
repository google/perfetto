---
name: What retains the most memory in a Java heap dump
tags: [android, memory, workflow]
runs: 3
files:
  - src: "{repo}/test/data/heap_graph_object_for_benchmarks.pftrace"
    dst: heap_dump.pftrace
---
We think a system process on our Android build is leaking Java memory. I
captured a Java heap dump with Perfetto: ./heap_dump.pftrace. What is
retaining the most memory, and is there evidence of a leak?
