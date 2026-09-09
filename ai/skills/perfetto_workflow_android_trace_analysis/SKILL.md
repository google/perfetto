---
name: perfetto-workflow-android-trace-analysis
description: Analyzes Perfetto traces to find the root cause of latency, memory, or
  jank issues in Android apps. Use when the user provides a Perfetto trace file and
  asks any question, ongoing investigation, or open-ended request to analyze its contents.
---

# Analyzing Perfetto Traces

This skill teaches you how to systematically analyze a Perfetto trace to
identify the root cause of performance issues like latency, jank, I/O stalls, or
memory pressure in Android apps.

If you have not yet loaded the trace into `trace_processor` or set up a query
session, follow the `perfetto_infra_querying_traces` skill first, then come back
here.

## Resources

- **Domain Hints:** Reference files for specific performance areas:
  [CPU](references/hints_cpu.md), [Graphics](references/hints_graphics.md),
  [I/O](references/hints_io.md), [IPC](references/hints_ipc.md),
  [Memory](references/hints_memory.md), [Power](references/hints_power.md).
  These files each contain multiple expert-vetted, powerful trace analysis
  techniques to steer and aid in the analysis.
- **Perfetto Standard Library:** Most common trace analysis queries (like thread
  context, scheduling, or app startup) are much simpler when using prebuilt
  stdlib modules rather than joining raw tables. Check the stdlib first.
  Reference: <https://perfetto.dev/docs/analysis/stdlib-docs>.
- **Query Setup and Execution:** Refer to the `perfetto_infra_querying_traces`
  skill for how to set up `trace_processor` (standalone or RPC mode), discover
  schemas, and execute queries against the trace.

## Setup Phase (Mandatory)

1. **Initialize Scratchpad (Chain of Evidence):**
   - Maintain your working memory in a local scratchpad file located in the
     exact same directory as the target trace file.
   - Name the file using the trace's filename appended with `_analysis.md`
     (e.g., `[trace_filename]_analysis.md`). Before creating it, check if a file
     with that name already exists by listing the directory's contents---to
     avoid biasing your investigation, DO NOT read the file's contents to check
     for its existence. If it does, append an incrementing version number (e.g.,
     `_v2.md`, `_v3.md`) until you find an available filename. You MUST hardcode
     this exact filename in all subsequent tool calls.
   - Use this scratchpad STRICTLY to log verified facts: timestamps, slice
     names, thread IDs (utid/tid), and thread states.
   - DO NOT write preliminary hypotheses or premature conclusions in the
     scratchpad. It is a strict Chain of Evidence.
2. **Review Domain Hints:** Read the Domain Hints in each file to get a high-
   level overview of what techniques are possible. Make sure to use this
   baseline knowledge when researching and retrieving hints during the ongoing
   investigation.
3. **Target Resolution:** If the user's request is broad (e.g., "why is the app
   slow?") and doesn't specify a package name:
   - Execute a query to identify the active application: `INCLUDE PERFETTO
     MODULE android.startup.startups; SELECT package FROM android_startups;`
   - If multiple packages are returned, ask the user to choose one. Save the
     chosen `package_name` to your scratchpad.

## Investigation Protocol

Follow this iterative loop until you have isolated the definitive root cause(s):

### 1. Formulate Hypothesis

- **Prioritization:** Form hypotheses using information from: user prompt \>
  "Domain Hints" ([CPU](references/hints_cpu.md),
  [Graphics](references/hints_graphics.md), [I/O](references/hints_io.md),
  [IPC](references/hints_ipc.md), [Memory](references/hints_memory.md),
  [Power](references/hints_power.md)) \> general knowledge. Be sure to
  leverage these "Domain Hints" as they are expert-vetted analysis techniques.
- **Source Attribution:** Explicitly mention the source of your hypothesis
  (e.g., "Based on hints_io.md...").
- **Focus Constraint:** Focus on the primary bottleneck. Avoid investigating
  deep into binder transactions unless the user explicitly asks for it or there
  is no other obvious bottleneck.
- **State Reasoning:** Briefly state your reasoning based on previous findings
  *before* generating a new query.

### 2. Plan and Collect Data

- **Metrics First:** Start with a high-level view using trace metrics (such as
  the 'android_startup' metric) to orient yourself before diving into custom
  SQL.
- **Broad to Narrow:** Begin with broad queries using minimal filters. Favor
  fuzzy matching (e.g., `GLOB '*abc*'`) over exact matching.
- **Overlapping Time:** When filtering by time, you MUST check for events that
  overlap with the target time range (e.g., `start1 < end2 AND start2 < end1`)
  to ensure you don't miss slices that span across the boundaries.

### 3. Analyze and Drill Down (Depth-First)

- **Evidentiary Rigor:** Do not draw conclusions without explicit data.
- **Wall Time vs. CPU Time:** Do not assume a long-running slice is actively
  computing. You MUST query the `thread_state` table for the exact timestamp
  window of suspicious slices to verify if the thread was `Running`, `Runnable`
  (waiting for CPU), or `Sleeping`/`Uninterruptible Sleep` (blocked).
- **Follow Dependencies:** If a thread is blocked/waiting, you MUST find what it
  is waiting *for* (Binder, Lock, I/O, etc.). Cross process boundaries if
  necessary. You cannot conclude an investigation on a waiting thread without
  identifying the blocker.

### 4. Exhaustive Investigation (Do Not Give Up Early)

- **Multiple Bottlenecks:** Complex performance issues rarely have a single
  cause. Do NOT stop your investigation after finding the first anomaly. Even if
  you find a major bottleneck (e.g., emulator graphics lag), you MUST continue
  searching for other independent system-wide issues (e.g., lock contention, I/O
  stalls). To find other bottlenecks, search through the content of the "Domain
  Hints" files ([CPU](references/hints_cpu.md),
  [Graphics](references/hints_graphics.md), [I/O](references/hints_io.md),
  [IPC](references/hints_ipc.md), [Memory](references/hints_memory.md),
  [Power](references/hints_power.md)) to retrieve and leverage expert-vetted,
  powerful trace analysis techniques. Investigate each relevant hint with depth.
- **Global Verification:** Periodically perform a system-wide query for the
  longest running slices (`ORDER BY slice.dur DESC`) and most frequent D-states
  to ensure your local investigation hasn't missed a massive, unrelated system
  stall.
- **Persist Through Dead Ends:** If a hypothesis is disproven or a query returns
  empty, do not conclude. Pivot your focus, broaden your search constraints
  (fuzzy matching, wider time windows), and continue the mission.

## Final Report

Only when you have followed the entire chain of dependencies to the root
cause(s) AND confirmed through exhaustive search that no other major bottlenecks
exist: 1. Summarize your findings detailing the verified chain of evidence 2.
Conclude with: "This concludes the trace analysis. You can review the full chain
of evidence in \[scratchpad_filename\]. Let me know if you would like me to
drill down into any of these specific threads, or if you'd like help drafting a
bug report."

## Reference

- Domain-Specific Hints:
  - [CPU Hints](references/hints_cpu.md)
  - [Graphics Hints](references/hints_graphics.md)
  - [I/O Hints](references/hints_io.md)
  - [IPC Hints](references/hints_ipc.md)
  - [Memory Hints](references/hints_memory.md)
  - [Power Hints](references/hints_power.md)
- PerfettoSQL Language Tour:
  <https://perfetto.dev/docs/analysis/perfetto-sql-getting-started>
- Trace Processor Reference:
  <https://perfetto.dev/docs/analysis/trace-processor>
- Generated Table Reference:
  <https://perfetto.dev/docs/analysis/sql-tables>
- Generated Standard Library Reference:
  <https://perfetto.dev/docs/analysis/stdlib-docs>
