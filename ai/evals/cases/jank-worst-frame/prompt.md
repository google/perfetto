---
name: Janky frames and the worst one
tags: [android, jank, frames, hard, stdlib]
runs: 3
files:
  - src: "{repo}/test/data/webview_jank.pb"
    dst: jank.pftrace
---
Users say the Gmail conversation list stutters while scrolling. I
captured ./jank.pftrace with Perfetto while reproducing it. How many of
Gmail's frames were janky, what kinds of jank does the trace attribute
them to, and which single frame was the worst?
