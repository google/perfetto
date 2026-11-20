---
provides: [query, multi_trace, python, record, write_file]
description: >-
  Analyze or record Perfetto traces to debug performance problems: slow app
  startup, jank and dropped frames, ANRs, high CPU, battery drain, memory
  growth, low-memory kills, and Java/native heap leaks, GPU utilisation,
  scheduling latency. Use for any trace file (.pftrace, .perfetto-trace,
  .pb, systrace, ftrace, Chrome JSON) from Android, Linux, Chrome or
  CUDA/GPU workloads, whether or not the user says "Perfetto". Bundles
  trace_processor (PerfettoSQL) plus guided workflows for Android memory
  and GPU analysis.
---
Do this once per session. Set `$SKILL_ROOT` to the absolute path of the
directory containing this `SKILL.md`; every path below is written relative
to it. Then put the bundled `trace_processor` on the `PATH` and check it
runs:

```sh
export SKILL_ROOT="/absolute/path/to/skills/perfetto"   # dir holding SKILL.md
chmod +x "$SKILL_ROOT/bin/trace_processor"
export PATH="$SKILL_ROOT/bin:$PATH"
trace_processor --version
```

This skill ships `trace_processor` and tells you how to drive it. The
first call downloads the native binary for this platform and caches it;
later calls are instant. Do not download `trace_processor` separately.
On Windows invoke it as `python "$SKILL_ROOT/bin/trace_processor" ...`.
