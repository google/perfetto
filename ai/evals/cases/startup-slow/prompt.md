---
name: Slow app cold start, Perfetto not mentioned
tags: [android, startup, trigger, no-mention]
runs: 3
files:
  - src: "{repo}/test/data/android_startup_real.perfetto_trace"
    dst: pixel_trace.pftrace
---
Gmail feels really slow to open on my Pixel. I recorded a system trace on
the phone while launching it; it's at ./pixel_trace.pftrace. How long did
the cold start actually take, and what was the app's main thread spending
that time on?
