---
name: perfetto
---

# Perfetto

Perfetto traces are analysed with `trace_processor`, a SQL engine over the
trace.

## Environment setup

{{environment_setup}}

## Pick the workflow

1. **Record a trace** (no trace file yet)
   - Android device: [recording_android_traces.md]($SKILL_ROOT/infra-references/recording_android_traces.md)
   - Linux host: [recording_linux_traces.md]($SKILL_ROOT/infra-references/recording_linux_traces.md)

2. **Memory** (RSS/swap, graphics & buffers, GC, LMK, heap dumps, profiles)
   - Memory in a system trace — RSS/swap, Java heap & GC, GPU/graphics/DMA-BUF, memory pressure, LMK (one trace or before/after): [system_trace_memory.md]($SKILL_ROOT/workflows/android_memory/system_trace_memory.md)
   - Leak or retention in one Java heap dump: [heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md)
   - Compare two Java heap dumps (before/after diff): [heap_dump_diff.md]($SKILL_ROOT/workflows/android_memory/heap_dump_diff.md)
   - Duplicate objects, caching opportunities: [heap_dump_caching_optimizer.md]($SKILL_ROOT/workflows/android_memory/heap_dump_caching_optimizer.md)
   - Many heap dumps, find common leak patterns: [heap_dump_cluster.md]($SKILL_ROOT/workflows/android_memory/heap_dump_cluster.md)
   - Native (C/C++) heap profile: [native_heap.md]($SKILL_ROOT/workflows/android_memory/native_heap.md)
   - Java allocation churn / GC pressure: [java_allocation_profile.md]($SKILL_ROOT/workflows/android_memory/java_allocation_profile.md)
   - Compare two native or Java heap profiles (before/after diff): [heap_profile_diff.md]($SKILL_ROOT/workflows/android_memory/heap_profile_diff.md)

3. **GPU / accelerator** (GPU-bound or host-bound?)
   - Which GPUs are in the trace: [gpu_info.md]($SKILL_ROOT/workflows/gpu/gpu_info.md)
   - Busy vs idle timeline, idle-gap attribution: [timeline_occupancy.md]($SKILL_ROOT/workflows/gpu/timeline_occupancy.md)

4. **Everything else** (startup, jank, ANRs, CPU, scheduling, counters,
   any custom question): read
   [querying.md]($SKILL_ROOT/infra-references/querying.md) and write
   PerfettoSQL.

Workflows carry their own queries and scripts; read `querying.md` only for
ad-hoc work.

## Reporting

Answer with numbers from the trace and the query that produced them, and
keep model-generated theories clearly separate from measured facts.
