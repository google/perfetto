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
[setup.md]($SKILL_ROOT/environment-references/setup.md)** (it sits next to this
file, in this skill's `environment-references/` directory). It defines how to
make `trace_processor` invokable and what to set `$SKILL_ROOT` to — the anchor
every other path in this skill (including the `$SKILL_ROOT/...` links here) is
written against. It is the only always-required file.

> [!NOTE] **After Installing `trace_processor`:** Run `trace_processor --version` as a smoke test to confirm installation, then refer to [querying.md]($SKILL_ROOT/infra-references/querying.md) for executing queries, starting the HTTP RPC server, or connecting via Python.

## 1. Are you trying to record a trace?

If you need to capture a new trace:

*   **On an Android device:** to record Java/native heap dumps, CPU stack
    samples, system traces, or a custom config via the Perfetto helper
    scripts, read
    [recording_android_traces.md]($SKILL_ROOT/infra-references/recording_android_traces.md).
*   **On Linux:** to record config-driven traces with `tracebox`, read
    [recording_linux_traces.md]($SKILL_ROOT/infra-references/recording_linux_traces.md).

## 2. Are you trying to solve memory issues?

If you have a resolved trace file and want to investigate memory leaks, object
retention, or optimize heap usage:

*   **Investigate Memory Leaks (Single Heap Dump):** To find memory leaks,
    identify what is keeping objects alive, and analyze retention chains using
    dominator tree analysis, read
    [heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md).
*   **Reduce Memory Waste (Optimize Heap):** To optimize general heap usage by
    finding duplicate objects (e.g., duplicate strings) or identifying caching
    opportunities, read
    [heap_dump_caching_optimizer.md]($SKILL_ROOT/workflows/android_memory/heap_dump_caching_optimizer.md).
*   **Fleet-wide Leak Analysis (Multiple Dumps):** To cluster multiple heap
    dumps (or a batch of dominator paths) to identify common leak patterns
    across a process, read
    [heap_dump_cluster.md]($SKILL_ROOT/workflows/android_memory/heap_dump_cluster.md).
*   **Investigate Native Memory Usage (Native Heap Profile):** To investigate memory leaks, active memory growth, or total allocation hot paths in C/C++ code using native heap profiles, read
    [native_heap.md]($SKILL_ROOT/workflows/android_memory/native_heap.md).
*   **Investigate Java Memory Churn (Java Allocation Profile):** To investigate memory churn, frequent GC lag, or temporary object allocations in Java/Kotlin code using allocation profiles, read
    [java_allocation_profile.md]($SKILL_ROOT/workflows/android_memory/java_allocation_profile.md).

## 3. Are you trying to analyze GPU/accelerator performance?

If you have a resolved trace with GPU activity and want to know whether the
workload is GPU-bound or host-bound:

*   **GPU inventory:** To see what GPUs the trace describes — vendor, model,
    architecture, per machine (multi-GPU and multi-machine aware), which decides
    which vendor-specific analysis applies, read
    [gpu_info.md]($SKILL_ROOT/workflows/gpu/gpu_info.md).
*   **GPU timeline occupancy:** To decompose the GPU timeline into device-busy
    vs idle time, get per-GPU busy percentages, and find the largest idle gaps
    with host-side attribution, read
    [timeline_occupancy.md]($SKILL_ROOT/workflows/gpu/timeline_occupancy.md).

## 4. Are you trying to do ad-hoc trace analysis?

If you want to load a trace and write custom PerfettoSQL queries:

*   Read [querying.md]($SKILL_ROOT/infra-references/querying.md) to learn about running
    one-shot queries, using the long-running RPC mode, discovering schemas, and
    writing efficient PerfettoSQL.

Workflows above are self-contained (they carry their own queries); read
`querying.md` only for ad-hoc work outside a workflow.

> [!IMPORTANT] **Query Authoring vs Execution Rules:**
> - When asked to write, draft, or explain a PerfettoSQL query without an active trace file path provided by the user, draft the complete query directly using stdlib patterns. Do NOT attempt shell execution of `trace_processor` on missing or placeholder trace files, and avoid repetitive repository code search loops.
> - When researching table or module schemas, use `__intrinsic_stdlib_modules` / `__intrinsic_stdlib_tables` (when connected to `trace_processor`) or refer to stdlib documentation (`https://perfetto.dev/docs/analysis/sql-tables`). Do NOT search unrelated application source code or general web search for trace processor table definitions.
> - When asked how to cluster multiple heap dumps or reduce noise across dumps, explain the clustering workflow (preprocessing paths, TF-IDF, K-Means clustering) and recommend running `scripts/cluster_paths.py` directly in your response.
> - When asked for next steps after running clustering or generating `clustered_output.csv`, respond directly recommending running `scripts/summarize_clusters.py clustered_output.csv report.html`. Do NOT attempt shell execution of Python scripts yourself.

## Finishing any analysis

Whichever path you took — a guided workflow or ad-hoc queries — end by
**saving a report of the analysis** as a markdown file in the working
directory (default name: `perfetto_analysis_report.md`, or where the user
asked). It should contain: the question investigated, the trace file(s)
used, the findings with concrete numbers, the key validated queries so the
analysis can be re-run, and open questions or suggested next steps. Tell
the user the report's path in your final message.
