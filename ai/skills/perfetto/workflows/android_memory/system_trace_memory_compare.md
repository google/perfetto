---
requires: [query, multi_trace]
---
# Comparing Android Memory Across Two System Traces

Follow
[system_trace_memory.md]($SKILL_ROOT/workflows/android_memory/system_trace_memory.md)
on both traces: load them as `before_tp` and `after_tp`, run every query on
both, and report `before -> after` for each counter.

**Comparable runs.** Compare the same process role and a comparable window:
trace durations and the number of workload repetitions can differ. Also check
that the device state matches: list the processes with `score <= 200` in
`android_oom_adj_intervals` in each trace. A foreground or perceptible process
present in only one run means the runs are not comparable, and its activity
(extra windows, media, notifications) can explain memory growth in system
processes without any regression in them.

**Per-trace differences.**
- Resolve the `upid` in each session; the same process has a different
  `upid` in each trace.
- Compare `heap_kb_after` between the two traces as well as across GCs.
- A trace with no GC has no live-heap floor, so comparing its
  `Heap size (KB)` minimum with a trace that did collect invents a
  retention delta.
- Compare each graphics counter across the runs rather than adding them.
