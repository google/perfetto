---
type: regex
flags: i
pattern: "2,?459|2[.,]4[56]\\d*\\s*MB|2[.,]40\\d*\\s*Mi?B"
---
The Java heap of `androidx.benchmark.integration.macrobenchmark.target` in `after.perfetto-trace` after its one
`Background young concurrent copying GC` is 2,459 KB (2.46 MB / 2.40 MiB),
down from 3,332 KB:
`SELECT s.ts, s.dur FROM thread_slice s WHERE s.process_name = 'androidx.benchmark.integration.macrobenchmark.target' AND s.name GLOB '* GC'` (with `INCLUDE PERFETTO MODULE slices.with_context`), then the first `Heap size (KB)` value of the app after the GC ends.
