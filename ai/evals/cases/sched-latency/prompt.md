---
name: Main-thread state breakdown during a cold start
tags: [android, sched, startup, hard]
runs: 3
files:
  - src: "{repo}/test/data/android_startup_real.perfetto_trace"
    dst: pixel_trace.pftrace
---
This Perfetto trace covers a cold start of Gmail on a Pixel
(./pixel_trace.pftrace). During the startup, how much time did Gmail's
main thread spend actually running on a CPU, how much sleeping, and how
much waiting runnable for a CPU? Give the breakdown in milliseconds, and
say whether scheduling latency is a meaningful part of the startup.
