---
type: regex
flags: i
pattern: "2,?331|2[.,]3[23]\\d*\\s*MB|2[.,]28\\d*\\s*Mi?B"
---
The Java heap of `androidx.benchmark.integration.macrobenchmark.target` in `before.perfetto-trace` after its one
`Background young concurrent copying GC` is 2,331 KB (2.33 MB / 2.28 MiB),
down from 3,148 KB:
`SELECT s.ts, s.dur FROM thread_slice s WHERE s.process_name = 'androidx.benchmark.integration.macrobenchmark.target' AND s.name GLOB '* GC'` (with `INCLUDE PERFETTO MODULE slices.with_context`), then the first `Heap size (KB)` value of the app after the GC ends.
