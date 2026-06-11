---
name: perfetto
description: >-
  Entry point for any task involving Perfetto traces — recording them or
  analyzing them. Analysis covers loading a trace, running PerfettoSQL queries,
  inspecting available tables/columns/stdlib, and guided investigations such as
  Android Java heap dumps (leaks, retention, clustering many dumps, caching to
  cut memory). Routes to the right workflow and reference files.
---

# Perfetto

This skill orchestrates Perfetto trace analysis.

## Prerequisites & Setup

**Before running any `trace_processor` command, read
[setup.md](environment-references/setup.md).** It defines how to invoke the
binary in this environment and how to install the `perfetto` Python client. It
is the only always-required file.

## 1. Are you trying to record a trace?

If you need to capture a new trace from an Android device:

*   To record Java/native heap dumps, CPU stack samples, system traces, or a
    custom config via the Perfetto helper scripts, read
    [recording_android_traces.md](infra-references/recording_android_traces.md).

## 2. Are you trying to solve memory issues?

If you have a resolved trace file and want to investigate memory leaks, object
retention, or optimize heap usage:

*   **Investigate Memory Leaks (Single Heap Dump):** To find memory leaks,
    identify what is keeping objects alive, and analyze retention chains using
    dominator tree analysis, read
    [heap_dump.md](workflows/android_memory/heap_dump.md).
*   **Reduce Memory Waste (Optimize Heap):** To optimize general heap usage by
    finding duplicate objects (e.g., duplicate strings) or identifying caching
    opportunities, read
    [heap_dump_caching_optimizer.md](workflows/android_memory/heap_dump_caching_optimizer.md).
*   **Fleet-wide Leak Analysis (Multiple Dumps):** To cluster multiple heap
    dumps (or a batch of dominator paths) to identify common leak patterns
    across a process, read
    [heap_dump_cluster.md](workflows/android_memory/heap_dump_cluster.md).

## 3. Are you trying to analyze GPU/accelerator performance?

If you have a resolved trace with GPU activity and want to know whether the
workload is GPU-bound or host-bound:

*   **GPU timeline occupancy:** To decompose the GPU timeline into device-busy
    vs idle time, get per-GPU busy percentages, and find the largest idle gaps
    with host-side attribution, read
    [timeline_occupancy.md](workflows/gpu/timeline_occupancy.md).

## 4. Are you trying to do ad-hoc trace analysis?

If you want to load a trace and write custom PerfettoSQL queries:

*   Read [querying.md](infra-references/querying.md) to learn about running
    one-shot queries, using the long-running RPC mode, discovering schemas, and
    writing efficient PerfettoSQL.

Workflows above are self-contained (they carry their own queries); read
`querying.md` only for ad-hoc work outside a workflow.
