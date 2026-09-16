---
name: Slowest binder calls on the main thread during a cold start
tags: [android, binder, startup, hard, stdlib]
runs: 3
files:
  - src: "{repo}/test/data/android_startup_real.perfetto_trace"
    dst: pixel_trace.pftrace
---
Perfetto trace of a Gmail cold start on a Pixel: ./pixel_trace.pftrace.
Which binder (IPC) calls did Gmail's main thread make during the cold
start, which system service handled them, and which ones cost the most
main-thread time? I want the top few by total duration.
