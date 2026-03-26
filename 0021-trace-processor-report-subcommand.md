# Trace Processor `report` Subcommand

**Authors:** @lalitm

**Status:** Draft

> **Note:** This document is a thought experiment exploring a possible future
> direction for trace_processor_shell. It is NOT a proposal for immediate
> implementation. The goal is to capture the design space and solicit feedback.

## Motivation

Perfetto traces are rich, multi-dimensional datasets. Today, extracting a
useful summary requires either:

1. Loading the trace in the UI and clicking around.
2. Writing ad-hoc SQL queries against trace_processor.
3. Authoring `TraceSummarySpec` textprotos for the `summarize` subcommand.

None of these serve the "I just collected a trace, what's in it?" use case
well. Users coming from `perf report` expect to point a tool at a data file
and immediately see an opinionated, useful summary — no query authoring, no
spec files, no UI.

This gap is especially felt by:

- **CLI power users** who want quick triage without leaving the terminal.
- **AI tools** that need structured trace summaries to reason about
  performance.
- **CI pipelines** that want a human-readable (or machine-parseable) trace
  summary as a build artifact.

The Firefox Profiler project is exploring a similar direction with their
experimental `pq` CLI tool (PR 5663 in the firefox-devtools/profiler repo),
which provides opinionated per-dimension views of profiling data from the
command line.

## Decision

Pending

## Design

### Relationship to `summarize`

`report` is a higher-level, opinionated cousin of `summarize`:

- **`summarize`** is the general-purpose engine — users author custom
  `TraceSummarySpec` protos to define exactly what to compute.
- **`report`** ships built-in specs that produce useful defaults across
  known trace dimensions.

Under the hood, `report` is built entirely on top of the summarization
machinery. Each dimension's report is a pre-authored `TraceSummarySpec` that
gets fed into the same engine that `summarize` uses.

### Built-in spec embedding

Report specs are authored as human-readable textproto files in the source tree
(e.g. `src/trace_processor/shell/report_specs/*.textproto`). A build rule
converts these to binary proto and embeds them as byte arrays in the binary,
following the existing `perfetto_cc_proto_descriptor` pattern used for metric
and trace descriptors. This means:

- Zero file I/O at runtime — specs are baked into the binary.
- Specs are human-editable in the source tree.
- The same build infrastructure that handles descriptor embedding is reused.

### CLI surface

```
trace_processor_shell report [dimension] [FLAGS] <trace_file>
```

When no dimension is specified, produce an overview covering all applicable
dimensions (skipping those with no data in the trace). When a dimension is
specified, produce a detailed per-dimension report.

#### Dimensions

| Dimension        | Description                                        |
| ---------------- | -------------------------------------------------- |
| `slices`         | Slice aggregations (wall duration, count, max)     |
| `stack-samples`  | CPU profiling samples (self/total time)            |
| `heap-profile`   | Heap allocation profiling (bytes, count)           |
| `heap-dump`      | Heap snapshot analysis (retained size, objects)    |
| `scheduling`     | Thread scheduling (CPU time, runnable, wait time)  |

#### Output format flags

```
--format text|json     Output format (default: text).
```

- **`text`**: Human-readable tables, similar to `perf report --stdio`.
- **`json`**: Structured JSON object, for tool/AI consumption.

#### Scoping flags

These filter the report to a subset of the trace data:

```
--pid <pid>            Scope to a specific process ID.
--process <name>       Scope to a process by name.
--tid <tid>            Scope to a specific thread ID.
--thread <name>        Scope to a thread by name.
--track <name>         Scope to a track by name.
--cpu <cpu>            Scope to a specific CPU.
--time <start>,<end>   Scope to a time range.
                       Accepts raw nanoseconds or human-friendly
                       format (e.g. 2.7s,3.1s).
```

Scoping flags are translated into structured query filters and
`interval_intersect` clauses in the underlying `TraceSummarySpec`, using the
existing DSL primitives — no raw SQL WHERE clauses.

#### Aggregation control

```
--top <N>              Number of entries per section (default: 10).
```

### Overview output

When invoked without a dimension, the overview produces a one-line trace
context followed by per-dimension aggregated highlights.

Example (`--format text`):

```
Trace: 12.3s | Android 14 | Pixel 7 Pro | 12 processes | 48 threads | 156 tracks

Slices (12.3M total):
  Name                          Count     Total dur   % of trace   Max dur
  Choreographer#doFrame         83.2k     4.1s        33.2%        128ms
  DrawFrame                     83.1k     3.5s        28.4%        96ms
  measure                       41.6k     890ms       7.2%         42ms
  layout                        41.6k     620ms       5.0%         38ms
  dequeueBuffer                 24.9k     310ms       2.5%         12ms
  eglSwapBuffers                24.9k     280ms       2.3%         8ms
  RenderThread::draw            24.9k     240ms       1.9%         6ms
  BinderTransaction             12.1k     180ms       1.5%         52ms
  animation                     8.3k      120ms       1.0%         4ms
  inflate                       2.1k      95ms        0.8%         18ms

Stack Samples (3.2k total):
  Function                                  Self%     Total%    Samples
  art::Thread::RunRootClock                 18.2%     42.1%     583
  __epoll_pwait                             12.1%     12.1%     387
  art::interpreter::Execute                 8.4%      31.2%     269
  ...

Scheduling:
  Thread                        CPU time   Runnable   Sleeping   % of trace
  RenderThread                  3.2s       120ms      8.9s       26.0%
  mali-cmar-backe               1.8s       45ms       10.4s      14.6%
  HeapTaskDaemon                890ms      12ms       11.3s      7.2%
  ...

Heap Profile: (not present in trace)
Heap Dump: (not present in trace)
```

### Per-dimension detail

Per-dimension reports provide a deeper view. For example,
`tp report slices <trace>` would show the same columns as the overview but
with a higher default `--top` and potentially additional breakdowns (e.g.
per-thread grouping).

The exact content of per-dimension reports is left as an open question for
now. As noted below, the call-tree views for stack samples (top-down /
bottom-up, as seen in `perf report` and the Firefox Profiler's `pq` tool)
are a natural fit here but the exact interaction model needs more thought.

### Per-dimension column definitions

#### Slices

Default aggregation key: slice name.

| Column       | Description                                          |
| ------------ | ---------------------------------------------------- |
| Name         | Slice name                                           |
| Count        | Number of instances                                  |
| Total dur    | Sum of wall durations across all instances            |
| % of trace   | Total duration as percentage of trace duration       |
| Max dur      | Maximum single-instance duration (outlier detection) |

#### Stack Samples

Modeled after `perf report --stdio`.

| Column    | Description                                             |
| --------- | ------------------------------------------------------- |
| Function  | Function name (symbol)                                  |
| Self%     | Samples where this function is at the top of the stack  |
| Total%    | Samples where this function appears anywhere in stack   |
| Samples   | Absolute sample count                                   |

#### Heap Profile

Same shape as stack samples but with bytes instead of sample count.

| Column       | Description                                          |
| ------------ | ---------------------------------------------------- |
| Allocator    | Allocation site / function                           |
| Self bytes   | Bytes allocated directly by this function             |
| Total bytes  | Bytes allocated by this function and its callees      |
| Count        | Number of allocations                                |
| Avg size     | Average allocation size                              |

#### Heap Dump

Point-in-time memory snapshot.

| Column        | Description                                         |
| ------------- | --------------------------------------------------- |
| Type/Alloc    | Type or allocator                                   |
| Retained size | Total retained memory                               |
| Live objects  | Count of live objects                                |

#### Scheduling

Per-thread scheduling summary.

| Column       | Description                                          |
| ------------ | ---------------------------------------------------- |
| Thread       | Thread name                                          |
| CPU time     | Total time spent running on a CPU                    |
| Runnable     | Total time in runnable state (waiting for CPU)       |
| Sleeping     | Total time sleeping                                  |
| % of trace   | CPU time as percentage of trace duration             |

### Sources of inspiration

- **`perf report`** (Linux perf): Opinionated defaults, hierarchical views,
  sort-by-overhead, `--stdio` output. The gold standard for "point at data,
  get useful summary."
- **Firefox Profiler `pq`**: CLI profile querying with per-dimension
  formatters, top-down/bottom-up call trees, scoping via time ranges, dual
  human/JSON output (PR 5663 in the firefox-devtools/profiler repo).
- **`pprof`** (Go): `-top`, `-text` views for CPU/heap profiles. Ergonomic
  top-N function summaries.
- **`heaptrack`** (KDE): CLI heap profile summaries — peak consumption, top
  allocators, leak candidates.

## Alternatives considered

### Ship report specs as external files

Pro:

* Users can inspect and modify specs without rebuilding.

Con:

* Requires distributing spec files alongside the binary.
* File discovery and path resolution adds complexity.
* Embedded binary protos are zero-overhead and follow existing precedent
  (metric descriptors, trace descriptors).

### Hardcode aggregation queries in C++

Pro:

* No proto serialization overhead.

Con:

* Loses the declarative nature of the summarization DSL.
* Cannot be reused by the `summarize` subcommand.
* Harder to maintain and review.

### Combine with `summarize`

Pro:

* One subcommand to learn.

Con:

* `summarize` is for custom specs; overloading it with opinionated defaults
  muddies its purpose.
* Different flag surfaces (scoping flags vs spec paths) would conflict.

## Open questions

* **Per-dimension drill-down interaction model:** For stack samples, top-down
  and bottom-up call trees (à la `perf report` and Firefox Profiler's `pq`
  tool) are a natural fit. Should these be sub-sub-commands
  (`tp report stack-samples top-down <trace>`), flags
  (`--view top-down`), or sections within the same output?
* **Exact per-dimension report content:** The overview columns are defined
  above. The detailed per-dimension reports may include additional breakdowns
  (e.g. per-thread slice grouping, per-process scheduling). Exact content
  TBD.
* **Spec authoring:** The built-in specs need to be written against the
  existing PerfettoSQL stdlib tables and modules. The exact table/module
  references for each dimension need to be determined.
* **Trace metadata extraction:** The one-line context line (OS, device,
  duration, process/thread/track counts) may require queries outside the
  summarization DSL. How to handle this cleanly?
