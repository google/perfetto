# Choose a profiler

Choose a Perfetto profiler to answer your CPU or memory question, or decide
whether tracing or an existing profile is a better fit.

## Compare the options

| Option | Best for | Platforms | Needs | What you get |
| --- | --- | --- | --- | --- |
| **CPU callstack sampling** (`linux.perf`) | Where CPU time goes, alongside other trace data | Android 15+, Linux | Profileable or debuggable app on user builds; `perf_event_paranoid` or root on Linux | Sampled callstacks on the timeline; flamegraphs for any time range |
| **Perf counters** (`linux.perf`) | Hardware and software event rates per CPU (cycles, instructions, cache misses) | Android, Linux | PMU access; stay within PMU capacity to avoid multiplexing | Counter tracks per CPU, queryable in SQL |
| **Import an existing profile** | Short, high-frequency profiles you already record with another tool | Any host with a browser | A pprof, `perf`, simpleperf or samply file | pprof on the Aggregate Profiles page; `perf` and simpleperf on the timeline |
| **Native heap profiling** (heapprofd) | Native (C/C++/Rust) allocations, churn and growth after profiling starts | Android 10+, Linux (`heap_profile host`) | Profileable or debuggable app on user builds | Allocation callstacks as flamegraphs: unreleased and total bytes and counts |
| **Java allocation profiling** (heapprofd, `com.android.art` heap) | Java/Kotlin allocation churn | Android 12+ | Profileable or debuggable app on user builds | Allocation callstacks; frees and GC are not tracked |
| **ART heap dumps** (`android.java_hprof`) | Java/Kotlin retention, leaks and class breakdowns | Android 11+ | Profileable or debuggable app on user builds; full HPROF needs a debuggable app | Object reference graph with retained sizes and dominators, in the Heap Dump Explorer |
| **Memscope** | Watching memory live, then profiling one process | Android, Linux | A Record page transport (WebUSB is simplest on Android) | Live system and per-process memory dashboard; one-click recording of heap dumps, smaps and native heap profiles |
| **Memory counters** | RSS, swap and system memory over time, LMKs | Android, Linux | `linux.process_stats`, `linux.sys_stats` or ftrace in the config | Counter tracks and events next to scheduling and other data |

TIP: Perfetto's CPU profiler is built for continuous, streaming collection. For
a short, high-frequency CPU profile, recording with `simpleperf` or `perf` and
opening the file in the UI is often simpler.

## Pick by situation

- You want to know which functions use CPU while the rest of the system is
  traced too → record callstack samples. See
  [Record CPU profiles and perf counters](/docs/getting-started/cpu-profiling.md).
- You want cycles, instructions or cache misses over time → record perf
  counters. See
  [Record CPU profiles and perf counters](/docs/getting-started/cpu-profiling.md#collecting-a-trace-with-perf-counters).
- You already have a Go pprof profile → open it directly. See
  [pprof profiles](/docs/getting-started/viewing-cpu-profiles.md#pprof).
- You recorded with `perf record` on Linux → convert with `perf script`, or
  bundle symbols with `trace_processor bundle`. See
  [Linux perf profiles](/docs/getting-started/viewing-cpu-profiles.md#linux-perf).
- You recorded with simpleperf on Android → open `perf.data` directly. See
  [simpleperf profiles](/docs/getting-started/viewing-cpu-profiles.md#simpleperf).
- You recorded with samply → you can open `profile.json`, but frames are not
  symbolized yet and show as addresses. See
  [samply profiles](/docs/getting-started/viewing-cpu-profiles.md#samply).
- Native memory keeps growing, or you want to cut `malloc` churn → use
  heapprofd. See
  [Profile native memory on Android](/docs/getting-started/memory-profiling.md)
  and [Native heap profiler (heapprofd)](/docs/data-sources/native-heap-profiler.md).
- You need the same on a Linux binary → use `heap_profile host`. See
  [Profile and trace native code on Linux](/docs/getting-started/linux-cookbook.md).
- Java/Kotlin code allocates too much and triggers GCs → profile Java
  allocations. See
  [Native heap profiler: ART allocation profiling](/docs/data-sources/native-heap-profiler.md#art-allocation-profiling).
- You suspect a Java leak, or need to know what holds the Java heap → take an
  ART heap dump and open it in the Heap Dump Explorer. See
  [Take an ART heap dump](/docs/how-to/art-heap-dump.md) and
  [Heap Dump Explorer](/docs/visualization/heap-dump-explorer.md).
- An app crashes with `OutOfMemoryError` (Android 14+) → capture a dump on
  OOM. See
  [Record boot traces and heap dumps on OOM](/docs/getting-started/local-android-trace-recording.md).
- You do not yet know which process is the problem → start with
  Memscope, then profile the process it points to. See
  [Monitor memory live with Memscope](/docs/visualization/memscope.md).
- You want memory use over time next to what the system was doing → record
  memory counters in a normal trace. See
  [Memory counters and events](/docs/data-sources/memory-counters.md).
- Your question is about latency, ordering or waiting (why a frame was late,
  what a thread was blocked on) rather than where resources went → trace
  instead of profiling. See
  [Tracing and profiling, explained](/docs/tracing-101.md).

## Related

- [Choose how to record](/docs/how-to/choose-how-to-record.md)
- [Open a pprof, perf, simpleperf or samply profile](/docs/getting-started/viewing-cpu-profiles.md)
- [Symbolize and deobfuscate profiles](/docs/learning-more/symbolization.md)
- [heap_profile](/docs/reference/heap_profile-cli.md)
