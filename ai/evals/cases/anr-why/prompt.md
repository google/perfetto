---
name: Which app ANR'd and why
tags: [android, anr, adhoc]
runs: 3
files:
  - src: "{repo}/test/data/android_anr.pftrace.gz"
    dst: anr.pftrace.gz
---
QA is reporting "app not responding" dialogs on our test devices. I have
a Perfetto trace from one device that covers the incident: ./anr.pftrace.gz.
Which app(s) hit an ANR, and what does the trace say the reason was?
