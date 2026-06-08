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

Router for Perfetto tasks. Find the matching row, read that file, follow it.

**Before running any `trace_processor` command, load
`environment-references/setup.md`** — it defines how to invoke the binary in
this environment. It is the only always-required file.

| If the user wants to… | Read |
|---|---|
| Investigate a **single** Android Java heap dump — memory usage, leaks, what retains an object | `workflows/android_memory/heap_dump.md` |
| Analyze **multiple** Android heap dumps (or a batch of dominator paths) for a process, to find common leaks | `workflows/android_memory/heap_dump_cluster.md` |
| Cut memory in a heap dump by caching / deduping repeated objects | `workflows/android_memory/heap_dump_caching_optimizer.md` |
| Run ad-hoc queries — load a trace, write PerfettoSQL, inspect tables/columns/stdlib | `infra-references/querying.md` |

Workflows are self-contained (they carry their own queries); read
`infra-references/querying.md` only for ad-hoc work outside a workflow.
