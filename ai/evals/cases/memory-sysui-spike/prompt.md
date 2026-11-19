---
name: SystemUI memory growth during Quick Settings media benchmark
tags: [android, memory, gpu, profiling, hard, workflow]
runs: 3
files:
  - src: "{repo}/test/data/sysui_qsmedia_microbenchmark.pb"
    dst: sysui_trace.pb
---
I recorded a Perfetto trace of a Quick Settings media benchmark on Android
(`./sysui_trace.pb`), and `com.android.systemui`'s memory footprint climbed
noticeably. Break down how its RSS/swap, Java heap, bitmaps, and GPU/HWUI
graphics memory changed across the trace, check whether the trace includes
usable heap profiling data to pinpoint allocations, and determine whether this
trace proves a Java or Bitmap memory leak.
