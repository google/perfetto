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
and immediately see an opinionated, useful summary: no query authoring, no
spec files, no UI.

This gap is especially felt by:

- **CLI power users** who want quick triage without leaving the terminal.
- **AI tools** that need structured trace summaries to reason about
  performance.
- **CI pipelines** that want a human-readable (or machine-parseable) trace
  summary as a build artifact, and to compare traces across builds.

## Decision

Pending

## Design

### Relationship to `summarize`

The two subcommands split by who decides what is computed:

- **`summarize`** is for summaries the user asks for themselves: they
  author `TraceSummarySpec` protos defining exactly what to compute.
- **`report`** is opinionated summaries we give the user: built-in views
  over the common trace dimensions, with no authoring required.

Under the hood both are PerfettoSQL. `summarize` converts query results to
protos as directed by the user's spec, with typed schemas and semantics
such as higher/lower-is-better; `report` is a presentation layer over the
PerfettoSQL standard library, where each view is computed by curated
stdlib queries and rendered from a hand-written proto schema
(`report.proto`, one message per view). Two things follow:

- **Typed output from a fixed proto.** `summarize` derives its output
  schema from the user's spec because the computation is arbitrary;
  `report`'s views are a closed, compile-time set, so a hand-written proto
  gives stable text/JSON output directly.
- **Report improvements land in the stdlib.** Every gap a report exposes is
  fixed in a module all SQL users benefit from, and the queries shown in
  the SQL drill-down hints are the same ones the tool runs.

### CLI surface

```
tp report [<noun> [<view>]] [FLAGS] (<trace_file> | --remote <addr>)
```

The first positional is always a noun (what data you are looking at); the
second is always a view (how to see it), from a closed per-noun set with a
default:

| Noun            | Views (default first)                        |
| --------------- | -------------------------------------------- |
| (none)          | overview                                     |
| `processes`     | `table`                                      |
| `tracks`        | `table`                                      |
| `slices`        | `table`, `histogram`, `timeline`, `inspect`  |
| `stack-samples` | `functions`, `top-down`, `bottom-up`         |
| `heap-profile`  | `functions`, `top-down`, `bottom-up`         |
| `heap-dump`     | `types`                                      |
| `scheduling`    | `summary`, `timeline`, `inspect`             |
| `counters`      | `list`, `timeline`, `histogram`, `inspect`   |

```
tp report trace.pftrace                          # overview
tp report stack-samples top-down trace.pftrace
tp report slices histogram --name doFrame --remote calm-blue-otter
tp report scheduling inspect --thread RenderThread --ts 2.734s trace.pftrace
tp report slices --baseline base.pftrace target.pftrace   # diff two traces
```

With no noun, the overview covers every noun that has data in the trace,
skipping the rest. `processes`, `tracks`, and `counters list` exist for
structural discovery: they enumerate the names that the scoping flags and
the other views take as input, so a user can orient in an unfamiliar trace
before drilling down. Output shape is statically known per (noun, view).

The nouns are deliberately generic trace primitives, applicable to any
trace regardless of origin. Domain-specific reporting (e.g. Android frame
timeline, ANRs, battery) is out of scope for now; it may be added in the
future.

#### Output format flags

```
--format text|json     Output format (default: text).
```

- **`text`**: human-readable tables, similar to `perf report --stdio`.
- **`json`**: one stable schema per (noun, view), generated from
  `report.proto`, with the scope context embedded and richer stats than
  text (e.g. p50/p95/p99 where text shows min/avg/max). Text output is
  explicitly not a stable interface; JSON is.

```
--show-sql             Print the stdlib queries the view executed.
```

`--show-sql` is what makes the SQL escape hatch concrete: the printed
queries are the ones the view ran, and a starting point for going beyond
what `report` offers.

#### Scoping flags

These filter the report to a subset of the trace data:

```
--pid <pid>            Scope to a specific process ID.
--process <name>       Scope to a process by name.
--tid <tid>            Scope to a specific thread ID.
--thread <name>        Scope to a thread by name.
--track <name>         Scope to a track by name.
--cpu <cpu>            Scope to a specific CPU.
--time <start>,<end>   Scope to a time range. Accepts raw nanoseconds or
                       human-friendly format (e.g. 2.7s,3.1s).
--ts <ts>              Scope to a point in time (same formats as --time).
                       Used by the inspect views.
```

Scoping flags are translated into structured filters on the underlying
stdlib queries. Every invocation carries its full scope in flags; there is
no sticky state, so commands are reproducible, CI-safe, and safe for
concurrent clients of one warm session.

#### Filtering and aggregation control

```
--name <glob>          Row filter, same meaning in every view. In
                       bottom-up, rows are roots, so the callers of
                       function X are: stack-samples bottom-up --name X.
                       Single-series views (histogram, timeline, inspect
                       on slices/counters) require the glob to match
                       exactly one distinct name; if it matches several,
                       the command fails and lists them. Many instances
                       of one name are fine (see inspect).
--min-duration <dur>   Hide rows below a duration threshold.
--min-count <N>        Hide rows with fewer instances.
--sort <key>           Sort key, enumerated per view (e.g. dur|count|max).
--top <N>              Entries per table section (default: 10).
--max-lines <N>        Node budget for tree views (default: 100).
--buckets <N>          Bucket count for timeline/histogram views.
--group-by <key>       Grouping key where a view supports one
                       (scheduling summary: thread (default) | process).
```

#### Warm sessions

```
--remote <addr>        Run against a warm session (see RFC 0031);
                       mutually exclusive with a trace path.
```

RFC 0031's `--remote` subcommand list grows `report`. The interactive
drill-down loop (overview -> view -> inspect) is a rapid sequence of
invocations against one trace, the pattern warm sessions exist to serve.

### Overview output

When invoked without a noun, the overview produces a one-line trace context
followed by per-noun highlights, ending with drill-down hints. The overview
accepts the scoping flags and `--top` (applied per section); `--name` and
`--sort` are rejected, since the sections have disjoint name spaces and
sort keys.

Example (`--format text`):

```
[trace.pftrace | full trace | 12.3s | Android 14 | Pixel 7 Pro | 12 processes]

Slices (12.3M total):
  Name                          Count     Total dur   % of trace   Max dur
  Choreographer#doFrame         83.2k     4.1s        33.2%        128ms @ 2.734s (RenderThread)
  DrawFrame                     83.1k     3.5s        28.4%        96ms @ 5.101s (RenderThread)
  measure                       41.6k     890ms       7.2%         42ms @ 8.913s (main)
  ...
  * 210 more slice names below --top 10; rerun with --top 50

Stack Samples (3.2k total):
  Function                                  Self%     Total%    Samples
  art::Thread::RunRootClock                 18.2%     42.1%     583
  __epoll_pwait                             12.1%     12.1%     387
  ...

Scheduling:
  Thread                        CPU time   Runnable   Sleeping   % of trace
  RenderThread                  3.2s       120ms      8.9s       26.0%
  mali-cmar-backe               1.8s       45ms       10.4s      14.6%
  ...

Heap Profile: (not present in trace)
Heap Dump: (not present in trace)

Next: tp report processes trace.pftrace
      tp report slices trace.pftrace --top 50
      tp report slices inspect --name "Choreographer#doFrame" --ts 2.734s --thread RenderThread trace.pftrace
```

### View semantics

#### Tree views

The tree views (`top-down`, `bottom-up`) are never dumped in full. Nodes
are included best-first under the `--max-lines` budget, scored by total%
with a depth decay (internal, not user-configurable). Single-child chains
are collapsed to the same indent, and pruned subtrees are summarized:

```
└─ ... (5 more children: combined 12.3%, max 4.1%)
```

`functions` and the tree views show both self and total, with self
displayed more prominently.

#### `timeline` and `histogram`

`timeline` buckets the trace (or the `--time` range) and renders one line
per bucket: an ASCII bar, the value, and the dominant entries in that
bucket. `scheduling timeline` needs no selection (CPU load per bucket,
plus the threads that dominate each bucket); `slices timeline` and
`counters timeline` take a `--name` and show
count and total duration, or average value, per bucket.

`histogram` renders the distribution of a single series: durations of a
slice name, or values of a counter. The output labels its scale (linear or
logarithmic) so bucket widths are not misread; JSON carries percentile
stats alongside the buckets.

#### `inspect`

`inspect` drills from a noun's aggregate into instances. The noun's normal
filters select the entity; scoping flags narrow which instances match. The
output shape is fixed per noun: aggregate stats over the matched instances
plus the instances themselves, rendered in full detail when exactly one
matches. `--name` must resolve to one distinct name (usually copy-pasted
from report output); many instances of that name are not an error: the
output is a wider instance table plus a hint on how to narrow.

- `slices inspect --name <n>`: instance count, duration percentiles, top
  instances by duration (each carrying its ts/thread for narrowing). A
  unique match shows dur, self dur, args, parent chain, and children.
- `counters inspect --name <n>`: value at `--ts`, plus min/avg/max and
  delta over the scoped range.
- `scheduling inspect --thread <t> --ts <ts>`: state, CPU, end state,
  waker thread, and wakeup latency: "why was this thread not running at T".

Adding an inspect view to a noun must pass a two-part test: (1) instances
are addressable by name or coordinates, never tool-emitted ids; (2) it
answers an instance-level question the aggregate views do not.

**Coverage gap:** `stack-samples`, `heap-profile`, and `heap-dump` have no
inspect view yet. A per-source function inspect (callers/callees of one
function) is tractable under this grammar, since the noun fixes the data
source, but is left as future work; heap-graph objects remain id-only.
Until then, the tree views plus `--name` and the SQL hints cover these.

### Diffing

Comparing two traces of the same workload (before/after a change, across
builds in CI) is an explicit goal. Any table view, and the overview,
accepts:

```
--baseline <trace|addr>   Compare against a baseline trace. Accepts a path
                          or a warm-session address (as with --remote).
```

Diff is a presentation-layer join: the view's queries run independently on
each trace, and the two typed results are joined on the view's aggregation
key (slice name, function, thread, process, track, or counter name) in the
shell. Neither engine ever sees both traces; multi-trace queries are out
of scope, both because they are very hard to reason about and because
sessions are single-trace (RFC 0031). Because the two runs are
independent, target and baseline may each be a path or a warm-session
address, in any combination.

Semantics:

- Value columns become Baseline / Target / Delta with signed formatting.
  Default sort is the duration delta; `--sort` selects others (e.g. count
  delta). Rows present on only one side are marked added/removed and rank
  by absolute delta.
- Deltas are raw values. The context header shows both trace durations and
  a footnote warns when they differ materially; `report` does not attempt
  to normalize for workload size and makes no claim of statistical
  significance.
- The join is by exact name. Canonicalizing names that embed dynamic
  values (tids, addresses) is an open question.
- Scoping flags apply to both traces identically.

```
[target.pftrace 10.1s | baseline base.pftrace 12.3s | full trace]

Slices (vs base.pftrace):
  Name                     Baseline dur   Target dur   Delta
  Choreographer#doFrame    4.1s           5.0s         +900ms
  inflate                  95ms           2ms          -93ms
  BitmapDecode             (absent)       310ms        +310ms
  * baseline is 22% longer than target; deltas are not normalized
```

`tp report --baseline base.pftrace target.pftrace` diffs the overview: a
per-noun digest of the biggest movements, the intended CI build artifact.
Tree and single-series views do not accept `--baseline` initially;
structural tree diffs need their own design (see open questions). A flag
is easier to miss than a subcommand, so `--baseline` is included in the
`report` help text's example block (as above).

### Per-view column definitions

#### Slices `table`

Aggregation key: slice name.

| Column     | Description                                             |
| ---------- | ------------------------------------------------------- |
| Name       | Slice name                                              |
| Count      | Number of instances                                     |
| Total dur  | Sum of wall durations across all instances              |
| % of trace | Total duration as percentage of trace duration          |
| Max dur    | Maximum single-instance duration, with the instance's   |
|            | ts/thread (outlier detection, feeds the inspect hint)   |

The noun covers sync and async slice tracks; instants (zero duration)
contribute to Count only. Durations sum per instance without overlap
deduplication, so % of trace can exceed 100 on async-heavy traces.

#### `stack-samples functions`

| Column   | Description                                             |
| -------- | ------------------------------------------------------- |
| Function | Function name (symbol)                                  |
| Self%    | Samples where this function is at the top of the stack  |
| Total%   | Samples where this function appears anywhere in stack   |
| Samples  | Absolute sample count                                   |

#### `heap-profile functions`

| Column      | Description                                          |
| ----------- | ---------------------------------------------------- |
| Function    | Allocation site / function                           |
| Self bytes  | Bytes allocated directly by this function            |
| Total bytes | Bytes allocated by this function and its callees     |
| Count       | Number of allocations                                |
| Avg size    | Average allocation size                              |

#### `heap-dump types`

| Column        | Description                                        |
| ------------- | -------------------------------------------------- |
| Type          | Type or allocator                                  |
| Retained size | Total retained memory                              |
| Live objects  | Count of live objects                              |

Retained size needs a dominator-tree computation and the Total%/Total
bytes columns need callstack ancestry walks; these are the most expensive
views to compute, and a further reason `report` benefits from warm
sessions.

#### `scheduling summary`

| Column     | Description                                           |
| ---------- | ----------------------------------------------------- |
| Name       | Thread (or process, with --group-by process)          |
| CPU time   | Total time spent running on a CPU                     |
| Runnable   | Total time in runnable state (waiting for CPU)        |
| Sleeping   | Total time sleeping                                   |
| % of trace | CPU time as percentage of trace duration              |

#### `processes` / `tracks` / `counters list`

Discovery tables: Process, PID, CPU time, % of trace, top threads; Track,
Type, Event count, Process/Thread; Counter name, Track/Process, Sample
count, Min, Avg, Max.

### Output contract

Every view emits:

1. **A context header** echoing the effective scope, derived purely from
   the invocation (see the overview example).
2. **Truncation footnotes**: what was hidden and the flag to reveal it.
3. **Next-command hints**: complete, copy-pasteable drill-down invocations
   (including `--remote` and scope flags), including `tp query` commands
   for raw data. The intended progression is: overview -> aggregate view
   -> inspect -> SQL.
4. **Terminal-aware tables**: columns are width-fitted when stdout is a
   TTY; piped or redirected output is untruncated.
5. **Errors fail fast**: an unknown noun/view combination, a missing
   required flag (e.g. `--ts` for `scheduling inspect`), or a malformed
   value exits non-zero with a message listing the valid options; with
   `--format json` the error is a structured object.

### Testing

Text output is locked by trace processor diff tests: one golden per
(noun, view) over reference traces, so any output change is explicit in
review and documented behavior cannot drift from shipped behavior. JSON is
validated against `report.proto` by construction.

## Alternatives considered

### Build on the summarize machinery

Pro:

- Reuses an existing declarative engine; specs shareable with `summarize`.

Con:

- Tree views have no representation in the flat-row summary output.
- Scoping flags rewrite the computation per invocation, so the
  "pre-authored spec" is really a template rewritten per invocation.
- Typed output does not require the engine: a fixed proto provides it.

### Alternative grammars

Two other grammars were drafted: a `--view` flag on a noun positional, and
view-first positionals with data-selection flags (`top-down
--heap-profile`). Both rejected: noun-then-view matches how users approach
a trace (pick the data, then the presentation) and how comparable
heterogeneous profiling tools have converged, keeps each view a closed
per-noun set, and has in-repo precedent (`tp server http`).

### Sticky server-side session state

Holding analysis state (a selected thread, zoom/filter stacks) in a
long-lived server makes interactive drill-down terse, but single
invocations become non-reproducible and concurrent clients unsafe. Warm
sessions (RFC 0031) already give the real benefit, parse-once performance,
so `report` keeps all state in flags.

### Id-based inspect addressing

`inspect slice:187432` style handles: rejected. Ids are only knowable if
the tool emitted them and are unstable across trace loads and versions;
names and coordinates (thread, ts) are legible from any output. Id-precision
lookups remain available via the SQL bridge.

### A `--focus <function>` stack filter

A pprof-style focus transform (keep stacks containing X, re-root per view):
rejected as a second drill-down mechanism with subtle semantics (recursion
double-counting, per-view re-rooting). `bottom-up --name` covers the
callers workflow; re-rooting is left as an open question.

### A separate `diff` subcommand

`tp report diff <base> <target>` was considered. Rejected: it would need to
replicate every noun/view under itself, whereas `--baseline` composes with
the existing views and the overview for free, and inherits scoping flags.

## Open questions

- **Domain-specific reports:** Android frame timeline, ANRs, battery and
  similar have stdlib modules but are out of scope for now; they may be
  added in the future.
- **Inspect for the callstack/heap nouns:** per-source function inspect
  (callers/callees) and heap-graph object drill-down (see coverage gap).
- **Tree diffs:** delta-annotated call trees under `--baseline`; the
  rendering and matching rules need their own design.
- **Per-trace time windows in diffs:** `--time` applies to both traces;
  comparing different windows (e.g. startup vs startup at different
  offsets) may need a `--baseline-time`.
- **Diff join canonicalization:** names embedding dynamic values (tids,
  addresses) break the exact-name join into spurious added/removed pairs.
- **Diff noise:** accepting multiple baseline traces (min/median/max) to
  separate real movement from run-to-run variance.
- **`--root <function>` on tree views:** pure re-rooting for "what does X
  call", if hop-wise navigation proves insufficient.
- **A heap-dump retainers view:** retaining paths for a type.
- **Cross-sectional view:** "what was happening at time T" across all
  nouns (the CLI analog of a vertical line in the UI).
- **JSON schema discovery:** how consumers find the per-view schemas
  (help section vs a dedicated subcommand).
