---
requires: [multi_trace]
---
# Comparing Android Memory Across Two System Traces

Follow
[system_trace_memory.md]($SKILL_ROOT/workflows/android_memory/system_trace_memory.md)
on both traces: load `before_tp` and `after_tp` (`--name before_tp`,
`--name after_tp`), run every query on both, and report `before -> after`
for each counter.

**Comparable runs.** Compare the same process role and a comparable window:
trace durations and the number of workload repetitions can differ. Also check
that the device state matches: list the processes with `score <= 200` in
`android_oom_adj_intervals` in each trace. A foreground or perceptible process
present in only one run means the runs are not comparable, and its activity
(extra windows, media, notifications) can explain memory growth in system
processes without any regression in them.
