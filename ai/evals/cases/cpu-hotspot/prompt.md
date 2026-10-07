---
name: CPU sampling hotspot
tags: [android, profiling, stacks, hard, stdlib]
runs: 3
files:
  - src: "{repo}/test/data/callstack_sampling.pftrace"
    dst: profile.pftrace
---
./profile.pftrace is a Perfetto CPU sampling profile from an Android
device. Which process got the most samples, and which function is the
hottest by self samples? Give sample counts.
