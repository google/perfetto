# Streaming finalized slices out of trace processor

**Authors:** @anubhavchaturvedi

**Status:** Draft

## Problem

Trace processor is two things at once: the most complete parser for the trace
formats Perfetto supports (protobuf TrackEvent, ftrace, Chrome JSON and more),
and an in-memory columnar database with a SQL engine on top. Many downstream
systems want the first without the second:

* Indexers and converters that turn traces into their own storage format
  (columnar files, a search index, a time-series store) and never query trace
  processor's tables.
* Batch and streaming pipelines that process many traces and care more about
  peak memory and CPU per trace than about ad-hoc queries.
* Tools with their own analysis model that need trace processor's parsing
  semantics (slice nesting, unfinished slices, flow resolution, arg
  translation, clock handling) to match what the Perfetto UI shows, but keep
  the results in their own data structures.

Today such a consumer has two unattractive options:

1. Let trace processor build its tables and read them back through SQL or the
   table API. Every slice is written into the slice table and its args into the
   global args table, then copied out again, and the whole trace stays resident
   at once. For example, a 41 MB protobuf trace already peaks at about 2.5 GB
   of RSS in `trace_processor_shell`.
2. Write and maintain a separate parser. This duplicates years of
   format-specific edge cases and drifts from Perfetto's semantics, so the same
   trace shows different slices in the downstream tool and in the UI.

We want these systems to reuse trace processor's parser directly and skip the
in-memory table formats they don't need, without changing anything for existing
users.

Out of scope:

* Replacing the slice table or offering a general "no tables" mode. This covers
  slices, their args, and the flow bookkeeping that depends on slice
  timestamps. Other tables (tracks, threads, processes, counters) are still
  built and are small by comparison.
* SQL access to slices when they are not written to the table. Query-time
  operators that read the slice table (`ancestor_slice`, `descendant_slice`,
  `connected_flow`, `experimental_slice_layout`) have nothing to read in that
  mode.
* A stable public API. The hook lives on `TraceProcessorContext`, so it targets
  embedders that build trace processor from source (see open questions).

## Decision

Pending

## Design

Add an optional `SliceSink` to `SliceTracker`. A sink receives every slice once
it is final, either alongside the slice table or instead of it. With no sink
installed, trace processor's tables, ids, args and stats are unchanged.

### Sink interface

```cpp
// Everything the slice table would hold for one slice, plus its args inline.
struct FinalizedSlice {
  SliceId id;
  int64_t ts;
  int64_t dur;  // -1 if the slice was still open at end of trace.
  TrackId track_id;
  StringId category;
  StringId name;
  uint32_t depth;
  std::optional<SliceId> parent_id;
  SliceThreadTiming thread;
  ArgsInserter::CompactArgSet args;
};

class SliceSink {
 public:
  enum class Mode { kAlongsideTable, kInsteadOfTable };
  virtual ~SliceSink();
  virtual void OnSliceFinalized(SliceId) {}        // kAlongsideTable
  virtual void OnSliceRecord(FinalizedSlice&&) {}  // kInsteadOfTable
};
```

* `kAlongsideTable`: the slice table is built as today, and the sink is told
  the id of each row once that row is final. This suits incremental consumers
  and doubles as a verification harness for the other mode.
* `kInsteadOfTable`: no slice rows and no global arg sets are written. The sink
  receives full records. Ids come from a storage-wide sequence
  (`TraceStorage::NextDetachedSliceId()`) that continues after any existing
  table rows, so they stay unique across trace and machine contexts the way
  table ids do.

### Installation

`TraceProcessorContext` gains an `on_slice_tracker_created` callback. It runs
for every `SliceTracker` regardless of trace format and is copied into forked
contexts. The embedder uses it to call `SliceTracker::SetSliceSink(sink, mode)`
before parsing starts.

### When a slice is final

A slice is delivered only after it has left the stack and after the caller has
patched the columns it sets once `End()` returns (thread duration, instruction
delta, a late rename). Delivery is therefore deferred to the next
`SliceTracker` operation, or to `FlushPendingSlices()` at end of trace.

* Order: pop order (children before parents, tracks interleaved), not timestamp
  order. Slices still open at end of trace follow the same rule.
* Deferred args: a slice whose args need translation becomes final only once
  translation runs at end of trace, so those slices arrive after all others.
* Re-entrancy: a sink that calls back into the `SliceTracker` from a callback
  is a fatal error.

### No slice table reads on the parsing path

For `kInsteadOfTable` to work, nothing on the parsing path may read or write a
slice row by id. This is the bulk of the change:

* `SliceTracker`'s open-slice stack holds every column, so the stack logic
  (nesting, depth, parent, unfinished slices) never reads the table. Args for
  an open slice are built lazily and committed on pop.
* Importers that patched slice rows by id go through the tracker instead,
  using `SetName`, `SetThreadTiming`, `SetThreadDeltas`,
  `ThreadTimingOfRecentlyEnded` and `StartTsOf`: the TrackEvent importer
  (thread timing for B/E/X events), the JSON parser, the GPU work-period
  tracker and the graphics-frame parser.
* `FlowTracker` records each flow source's start timestamp instead of reading
  it back from the table, so it can still order a flow's endpoints. When the
  direction can't be determined without a table, it records a stat rather
  than guessing.
* `ArgsTracker::AddArgsDetached` builds a slice's arg set without binding it to
  a table cell; that is how a record carries its args inline.
* TrackEvent extension parsers already receive the event timestamp through
  `TrackEventFieldContext`, so plugins need no change. The header documents
  that a slice id names no table row in `kInsteadOfTable` mode.

### Safety rails

* `SliceTracker::WritesTable()` tells callers whether ids index the table.
* Switching back to writing the table once detached ids have been issued is
  fatal, since new rows would reuse those ids. Changing mode while any slice is
  open is also fatal.
* `StartTsOf` is fatal when it can't answer without a table, so a future
  importer that reads slice rows by id fails loudly in tests rather than
  silently returning wrong data.

## Validation

A prototype is applied on top of current `main`.

Tests:

* 21 new `SliceSinkTest` unit tests cover delivery in both modes, deferred
  translation, detached id allocation, mode switching, re-entrancy and thread
  timing.
* All of `perfetto_unittests` passes, and all trace processor diff tests pass
  except one symbolizer test that fails identically on `main` in the test
  environment.

No-sink equivalence, comparing `main` against the prototype with no sink
installed, on sample PyTorch profiler traces:

| Trace                  | Slices    | Args       | Flows   |
| ---------------------- | --------- | ---------- | ------- |
| 41 MB protobuf         | 2,017,443 | 29,262,962 | 261,639 |
| 51 MB JSON (gzip)      | 2,093,340 | 25,249,685 | 283,384 |

Full sorted dumps of `slice`, `args` and `flow` are byte-identical. `stats`
differs only in the wall-clock `parse_trace_duration_ns` and
`guess_trace_type_duration_ns` entries.

No-sink cost (`trace_processor_shell`, 10 paired runs per trace, alternating
order, median of per-pair CPU-time deltas):

| Trace             | `main` | Prototype | Delta  | Peak RSS          |
| ----------------- | ------ | --------- | ------ | ----------------- |
| 41 MB protobuf    | 5.83 s | 6.09 s    | +2.9%  | 2,479 → 2,473 MB  |
| 51 MB JSON (gzip) | 8.30 s | 8.21 s    | -0.1%  | 2,280 → 2,295 MB  |

The protobuf regression is consistent (9 of 10 pairs slower). The likely
cause is the larger open-slice stack entry (all columns plus thread timing)
and the larger flow map value; this has to be fixed before landing (see open
questions). `kInsteadOfTable` itself has not yet been benchmarked end to end
against reading the tables back; that needs an embedder harness and will be
added, including on a multi-GB protobuf trace.

Known gaps in the prototype, found in review:

* Thread timing recorded through `use_async_tts` still goes to
  `virtual_track_slices` and is not part of `FinalizedSlice`. Rows in other
  tables that refer to slices (flows, TrackEvent callstacks, GPU correlation)
  carry ids with no slice row in `kInsteadOfTable` mode. Both need to be
  delivered or documented.
* Flows whose direction can't be resolved without a table reuse the
  `flow_without_direction` error stat; they need a dedicated stat.
* The re-entrancy check covers the calls that drain the queue but not
  `SetName`, `SetThreadTiming` or `SetThreadDeltas`.
* A process-wide counter of remaining slice table reads in `FlowTracker` is a
  prototype aid and will be removed or made per-context.

## Alternatives considered

### Read the tables back after parsing

Pro:

* Works today with no trace processor changes.

Con:

* Pays for materializing and then copying every slice and arg.
* Holds the whole trace in memory at once, which limits trace size and
  parallelism in batch pipelines.

### A separate lightweight parser

Pro:

* No coupling to trace processor internals.

Con:

* Duplicates format-specific logic that trace processor already gets right.
* Diverges from what the Perfetto UI shows for the same trace.

### Callbacks in each importer

Pro:

* Each importer controls exactly what it reports.

Con:

* Every importer needs its own hook, and consumers would have to reimplement
  nesting, unfinished-slice and arg-translation rules.
* `SliceTracker` is the single place where all of those are already resolved,
  so a sink there sees exactly what the table would have held.

## Open questions

* Should the sink be reachable through the public `TraceProcessor` / `Config`
  API, or stay an embedder-only hook on `TraceProcessorContext`?
* Is pop order acceptable for consumers, or should an optional
  timestamp-ordered delivery mode be offered at a buffering cost?
* Should the same pattern extend to other large tables (counters, sched) in a
  follow-up, or are slices enough?
* How should the no-sink cost be removed: keep the full-column stack only when
  a sink is installed, shrink `SliceInfo` (for example by storing thread timing
  out of line), or both?
