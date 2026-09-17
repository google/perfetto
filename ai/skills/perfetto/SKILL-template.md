---
name: perfetto
description: >-
  Analyze or record Perfetto traces to debug performance problems: slow app
  startup, jank and dropped frames, ANRs, high CPU, battery drain, memory
  growth and Java/native heap leaks, GPU utilisation, scheduling latency.
  Use for any trace file (.pftrace, .perfetto-trace, .pb, systrace, ftrace,
  Chrome JSON) from Android, Linux, Chrome or CUDA/GPU workloads, whether or
  not the user says "Perfetto". Bundles trace_processor (PerfettoSQL) plus
  guided workflows for Android memory and GPU analysis.
---

# Perfetto

Perfetto traces are analysed with `trace_processor`, a SQL engine over the
trace. This skill ships that tool and tells you how to drive it.

## Setup (once per session)

Set `$SKILL_ROOT` to the absolute path of the directory containing this
`SKILL.md`; every path below is written relative to it. Then put the
bundled `trace_processor` on the `PATH` and check it runs:

```sh
export SKILL_ROOT="/absolute/path/to/skills/perfetto"   # dir holding SKILL.md
chmod +x "$SKILL_ROOT/bin/trace_processor"
export PATH="$SKILL_ROOT/bin:$PATH"
trace_processor --version
```

The first call downloads the native binary for this platform and caches
it; later calls are instant. Do not download `trace_processor` separately.
On Windows invoke it as `python "$SKILL_ROOT/bin/trace_processor" ...`. If
the environment already mandates its own `trace_processor` (Google
internal, OEM or CI images), use that instead.

## Pick the workflow

1. **Record a trace** (no trace file yet)
   - Android device: [recording_android_traces.md]($SKILL_ROOT/infra-references/recording_android_traces.md)
   - Linux host: [recording_linux_traces.md]($SKILL_ROOT/infra-references/recording_linux_traces.md)

2. **Memory** (heap dumps, leaks, churn)
   - Leak or retention in one Java heap dump: [heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md)
   - Duplicate objects, caching opportunities: [heap_dump_caching_optimizer.md]($SKILL_ROOT/workflows/android_memory/heap_dump_caching_optimizer.md)
   - Many heap dumps, find common leak patterns: [heap_dump_cluster.md]($SKILL_ROOT/workflows/android_memory/heap_dump_cluster.md)
   - Native (C/C++) heap profile: [native_heap.md]($SKILL_ROOT/workflows/android_memory/native_heap.md)
   - Java allocation churn / GC pressure: [java_allocation_profile.md]($SKILL_ROOT/workflows/android_memory/java_allocation_profile.md)

3. **GPU / accelerator** (GPU-bound or host-bound?)
   - Which GPUs are in the trace: [gpu_info.md]($SKILL_ROOT/workflows/gpu/gpu_info.md)
   - Busy vs idle timeline, idle-gap attribution: [timeline_occupancy.md]($SKILL_ROOT/workflows/gpu/timeline_occupancy.md)

4. **Everything else** (startup, jank, ANRs, CPU, scheduling, counters,
   any custom question): read
   [querying.md]($SKILL_ROOT/infra-references/querying.md) and write
   PerfettoSQL against a warm session.

Workflows carry their own queries and scripts; read `querying.md` only for
ad-hoc work.

## Reporting

Answer with numbers from the trace and the query that produced them, and
keep model-generated theories clearly separate from measured facts.

For any investigation that took more than a handful of queries, or that
followed a workflow, also save a markdown report in the working directory
(default `perfetto_analysis_report.md`, or where the user asked) so a
human can follow what was done without reading the whole session. It
should contain the question, the trace file(s), the findings with
concrete numbers, the validated queries so the analysis can be re-run,
and open questions or next steps. Mention its path in your final message.
A single quick question answered by one or two queries does not need a
file.
