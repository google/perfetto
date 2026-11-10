---
name: Distinguish heavy GC allocation churn from a memory leak
tags: [android, memory, hard, workflow, trigger, no-mention]
runs: 3
files:
  - src: "{repo}/test/data/android_anr.pftrace.gz"
    dst: incident.pftrace.gz
---
`com.google.android.gms.persistent` (pid 30647) ran multiple long garbage
collections in `./incident.pftrace.gz`, and a teammate suspects a Java or
Bitmap memory leak (or swap growth). Analyze its RSS and swap, `Heap size (KB)`
before and after each GC, what triggered the `Explicit` GCs, and its
bitmap/binder counters, and determine whether the process is leaking memory or
churning temporary allocations.
