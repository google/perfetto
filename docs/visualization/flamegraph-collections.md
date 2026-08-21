# Flamegraph Collections

A trace often contains not one flamegraph but many: an archive with hundreds
of pprof files, CPU samples from dozens of threads, or heap profiles from
several processes. For these, the Perfetto UI shows a **flamegraph
collection**: a filterable grid with one row per profile above a flamegraph.
The grid's filters pick the working set; a **Merge** switch either sums the
selection into a single combined flamegraph or steps through the profiles one
at a time.

The same view backs three surfaces:

- The **Aggregate Profiles page**, for archives of pprof or collapsed-stack
  files.
- The **stack-sample flamegraph tabs** shown for an area selection over
  sampling-profiler tracks (Linux perf, Instruments, Chrome, Simpleperf,
  Gecko, perf-script text, legacy V8).
- The **heap-profile flamegraph tabs** shown for an area selection over
  heapprofd / ART allocation-sampling tracks.

This guide covers:

- [The collection view](#the-collection-view): the grid, merging, stepping
  and filtering.
- [pprof and collapsed-stack archives](#pprof-and-collapsed-stack-archives):
  building an archive and exploring it.
- [Stack samples](#stack-samples): per-thread/process drill-down of an area
  selection.
- [Heap profiles](#heap-profiles): comparing allocations across processes.
- [Querying the data](#querying-the-data) with PerfettoSQL.

## The collection view

![The Aggregate Profiles page for an archive of four real Android
profiles (system_server and surfaceflinger CPU samples, two native heap
profiles): a grid with one row per profile and per-sample-type total
columns, the Merge switch on ("Merging 4 of 4 profiles"), and one combined
flamegraph below.](../images/flamegraph_collections/pprof-merged.png)

The view is split by a draggable divider:

- **The grid** (top) has one row per profile. The first column identifies the
  profile (file name, thread, process); the other columns are that profile's
  totals (samples, bytes, nanoseconds), so you can see at a glance where the
  weight is before opening a single flamegraph.
- **The flamegraph** (bottom) shows the current working set, with the usual
  controls: the *Measure* selector to switch metric, Top Down / Bottom Up,
  the filter bar (`Show Stack`, `Hide Stack`, `Hide Frame`, `Show From
  Frame`, `Pivot`) and Export.

### The working set

The grid is not just a table — it *selects*. Add a filter (from a column
header menu, or by clicking a cell) and the flamegraph immediately reflects
only the matching profiles. The summary next to the Merge switch always says
what you are looking at, e.g. `Merging 37 of 214 profiles`.

Typical filters (using the archive from the screenshots):

- `profile glob *.cpu.pprof` — only the CPU profiles.
- `samples (count) > 500` — only profiles with more than 500 samples.
- Click a value in an identifier column to filter to (or exclude) it.

### Merge on: one combined flamegraph

With **Merge** on (the default) the working set is summed into a single
flamegraph. Identical stacks from different profiles combine into one node,
because call stacks are interned globally when the trace is imported — so
"which code path is hot *across the whole fleet of profiles*" is a single
view, not something you eyeball profile by profile.

### Merge off: step through profiles

![Merge off on the same archive: "Showing 4 of 4 profiles", a 3 / 4
position indicator with previous/next chevrons, system_server.cpu.pprof
highlighted in the grid, and its own CPU flamegraph
below.](../images/flamegraph_collections/pprof-step.png)

With **Merge** off you get one flamegraph per profile and step between them:

- the **‹ ›** chevrons or the **left/right arrow keys** move to the
  previous/next profile in the grid's sort order;
- **clicking a profile name in the grid** jumps straight to it (also leaving
  merge mode if it was on);
- the shown profile is highlighted in the grid, and a `3 / 37` indicator
  shows where you are.

The selected measure, view (top-down/bottom-up) and flamegraph filters are
shared across profiles, so stepping compares like with like: set up `Hide
Stack: malloc` once and every profile you step through has it applied.
Sorting the grid changes the stepping order — sort by a total descending to
walk profiles from heaviest to lightest.

## pprof and collapsed-stack archives

A single pprof (or collapsed-stack) file opens on the Aggregate Profiles
page as one flamegraph with a metric selector. To analyze *many* profiles
together, put them in a zip or tar archive and open the archive:

```bash
# Any mix of pprof / collapsed-stack files works; members may be gzipped
# (as Go's pprof writes them by default).
zip profiles.zip *.pprof
# or
tar cf profiles.tar *.pprof
```

Profiles come from any pprof-writing profiler (Go's `runtime/pprof`,
`perf_to_profile`, ...). Perfetto traces are themselves a source:
`traceconv profile` extracts real pprofs from traces containing heapprofd
or perf-sampling data. That is exactly how the archive in the screenshots
was built — from real captures: traced_perf CPU sampling of Android's
system_server and surfaceflinger, plus two heapprofd native-heap dumps:

```bash
traceconv profile --perf cpu_sampling_trace.pftrace  # one CPU pprof per process
traceconv profile heapprofd_trace.pftrace            # one heap pprof per dump
# traceconv prints its output directory; name the profiles and archive them:
tar cf android_profiles.tar system_server.cpu.pprof surfaceflinger.cpu.pprof \
    system_server.heap.pprof heapprofd_example.heap.pprof
```

Drag the archive into [ui.perfetto.dev](https://ui.perfetto.dev). Each
member is imported as its own profile, scoped by its file name inside the
archive; since profile-only traces have no timeline, the UI lands directly
on the Aggregate Profiles page with the collection view.

Each distinct sample type found across the archive (`samples (count)`,
`Total size (bytes)`, `Unreleased size (bytes)`, ...) becomes both a grid
column and a flamegraph measure. A profile that lacks a sample type simply
has an empty cell — the CPU profiles above have no heap columns and vice
versa — and contributes nothing when that measure is merged.

NOTE: Frames, mappings and symbols are interned across the whole archive, so
opening hundreds of profiles with similar stacks does not multiply memory
usage by the profile count.

## Stack samples

For traces with sampling-profiler data, select an area over the sample
tracks (or run the `Select all perf samples` command from the omnibox) and
open the source's flamegraph tab, e.g. **Perf Sample Flamegraph**:

![A real Android trace with all perf samples selected: the Perf Sample
Flamegraph tab shows a context grid — system_server and surfaceflinger
rows with their sample counts and cpu-clock totals — above the merged
flamegraph.](../images/flamegraph_collections/stack-samples.png)

Here an entry is an **execution context**: a thread or process contributing
samples to the selection (narrowed to a profiling session where the trace
has several). The grid shows each context's sample count and, where the
profiler recorded counters (e.g. `cpu-cycles`), the per-counter totals over
the selected time range.

Merge on reproduces the classic behaviour — one flamegraph over everything
selected. Merge off answers the questions merging hides: *does thread B have
the same profile shape as thread A?* Step between them with the arrow keys,
or filter the grid down to the two or three contexts you care about before
merging just those.

## Heap profiles

For heapprofd (native malloc), ART allocation sampling and custom heapprofd
allocators, the flamegraph tab for an area selection supports selecting
**multiple processes**: the grid gets one row per process with its dump
count and total allocation size/count over the selected window.

- Merge on: one flamegraph of allocations across all selected processes.
  Unreleased-memory accounting stays correct per process — an allocation
  freed in process A never cancels one made in process B, even when both
  happened at the same call path.
- Merge off: step process by process with a shared measure (e.g.
  `Unreleased Malloc Size`) to compare which call paths retain memory in
  each.

Clicking a single heap-profile diamond on a track still opens the classic
single-snapshot panel with its Download (pprof) option.

## Querying the data

Everything the collection shows is derived from trace processor tables, so
it can be reproduced (and extended) in the **Query (SQL)** page. For pprof
archives, profiles live in `__intrinsic_aggregate_profile` (one row per
profile × sample type, `scope` = the archive member name) and their samples
in `__intrinsic_aggregate_sample`:

```sql
SELECT
  p.scope AS profile,
  p.sample_type_type AS metric,
  p.sample_type_unit AS unit,
  sum(s.value) AS total
FROM __intrinsic_aggregate_profile p
JOIN __intrinsic_aggregate_sample s ON s.aggregate_profile_id = p.id
GROUP BY p.id
ORDER BY p.scope, p.sample_type_type;
```

```
"profile","metric","unit","total"
"heapprofd_example.heap.pprof","Total count","count",48.000000
"heapprofd_example.heap.pprof","Total size","bytes",1416.000000
"heapprofd_example.heap.pprof","Unreleased count","count",48.000000
"heapprofd_example.heap.pprof","Unreleased size","bytes",1416.000000
"surfaceflinger.cpu.pprof","samples","count",201.000000
"system_server.cpu.pprof","samples","count",572.000000
"system_server.heap.pprof","Total count","count",217.000000
"system_server.heap.pprof","Total size","bytes",1114008.000000
"system_server.heap.pprof","Unreleased count","count",8.000000
"system_server.heap.pprof","Unreleased size","bytes",84848.000000
```

Merging profiles in SQL is a `WHERE ... IN` away — the same trick the UI
uses — because samples from every profile reference one shared,
globally-interned callsite tree (`stack_profile_callsite`). Stack samples
and heap profiles are similarly queryable through the `stack_sample` and
`heap_profile_allocation` tables.

## For plugin authors

The view is a reusable component:
[`ui/src/components/flamegraph_collection.ts`](https://github.com/google/perfetto/blob/main/ui/src/components/flamegraph_collection.ts).
A plugin supplies rows, columns, a stable per-entry key and one function —
`metricsForKeys(keys)`, returning the flamegraph metrics that sum the given
entries. The component derives both modes from it: merge is
`metricsForKeys(allWorkingKeys)`, stepping is `metricsForKeys([key])` per
entry. Grid state, working-set computation, navigation, keyboard handling
and flamegraph-state persistence come for free. See the pprof
(`dev.perfetto.AggregateProfiles`), stack-sample (`dev.perfetto.StackSamples`)
and heap-profile (`dev.perfetto.HeapProfile`) plugins for the three existing
integrations.
