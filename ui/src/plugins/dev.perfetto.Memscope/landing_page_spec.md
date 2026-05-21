# Memscope Landing Page Spec

## Overview

The memscope landing page is designed to show up first when opening a trace
which is predominantly memory focused. It displays a general overview of memory
in the trace, and is designed to be more approachable compared to dumping the
user in the timeline. It will provide some high level overviews based on smaps,
java heap dumps and native heap dumps, as well as providing links to the
timeline and other memory views where available.

## Design

### Landing Page and Process Selector

- Loads the processes that have one or more of the following:
  - Smaps dumps (profiler_smaps)
  - Java heap dumps (heap_graph_object)
  - Native heap dumps (heap_profile_allocation)
- Displays a select dropdown at the top of the page showing the name and pid of
  each candidate process and the counters of the above.
- The subpage is the upid of the selected process.
- If no process is selected - we choose the 'best' process to show details about
  (e.g. the one with the most snapshots).
- When a process is selected (or auto), we show a little process card displaying
  the process name, and some stats about the trace (time, num snapshots, etc).
- Two tabs dispayed:
  - Memory overview
  - Smaps details

### Memory Overview Tab

#### Billboards

##### Uptime

Uptime requires that we enable record_process_age.

```
data_sources {
  config {
    name: "linux.process_stats"
    process_stats_config {
      record_process_age: true
    }
  }
}
```

We can obtain this value from the process table via the `start_ts` field.

```sql
SELECT start_ts FROM process WHERE upid = ${upid}
```

##### OOM Score

OOM score can be obtained via various methods - we obtain it via polling:

```
data_sources {
  config {
    name: "linux.process_stats"
    process_stats_config {
      proc_stats_poll_ms: 1000
    }
  }
}
```

Read the final sample sample of the 'oom_score_adj' counter for the relevant
process:

```sql
SELECT c.value AS oom_score_adj
FROM counter c
JOIN process_counter_track t ON c.track_id = t.id
WHERE t.name = 'oom_score_adj' and t.upid = ${upid}
ORDER BY c.ts desc
LIMIT 1
```

##### Peak RSS anon+swap

Smaps is required - right now we can only do this with an extra config passed to
the java heap prof datasource:

```
{
  config: {
    name: 'android.java_hprof',
    javaHprofConfig: {
      pid: [pid],
      continuousDumpConfig: {
        dumpIntervalMs: DUMP_INTERVAL_MS, // Required for Java profiles.
      },
      smapsConfig: {},
    },
  },
},
```

For each snapshot, sum anonymous_kb and swap_kb from each snapshot and find the
max.

```sql
SELECT s.ts, CAST(ifnull(SUM(s.anonymous_kb + s.swap_kb), 0) * 1024 AS INT) AS total
FROM profiler_smaps s
WHERE s.upid = ${upid}
GROUP BY s.ts
ORDER BY s.ts ASC
```

##### RSS Spike

This is designed to capture any spike in RSS missed in the periodic RSS polled
counters. It's entirely counter based and

To enable - configure process stats polling. The mem.rss.watermark and mem.rss
counters should be present for each process. We then take the difference of the
max watermark (or the last watermark, but choose max to be safe) from the max
rss. This gives us the delta between the highest rss the kernel ever saw vs what
we recorded.

##### Memory Delta

Derived entirely from smaps - same as the peak RSS ANON+SWAP card. It takese the
rss anon + swap from the first snapshot in the trace and takes it away from the
last snapshot in the trace to get the delta.

##### Trend

Same as above but normalized over time to give a value in MiB / hour.

##### GC Churn & Java Churn

Not available / implemented yet.

#### Trace Overview

Trace overview a stacked line chart showing the composition of smaps snapshots
over time. Broken down by:

- Native: [anon:scudo*], libc_malloc, jemalloc, GWP-ASan, [heap]
- Java: _dalvik_, ashmem dalvik (the managed heap)
- File-backed: any path starting / (.so, .oat, .apk, etc.)
- Graphics: kgsl/mali/dri devices, dmabuf
- Thread stacks: [stack], [anon:stack*]
- Other: everything else

For each section, the rss_kb + swap_kb (not anonymous_kb).

It's used as a way to get a rough sense of how the memory was growing over the
course of the trace, though it's rather coarse becuase it's based on smaps. It's
also used as a navigation tool allowing the user to click on a snapshot to see
more information about it in the folloiwng sections - or drag to select a region
of snapshots and see the diffs as well.

#### Where did all the memory go?

A mini flamegraph showing in more (but still limited) detail the snapshot
selected in the grpah above. The sizes of the flamegraph are always absolute (of
the later snapshot if in diff mode). Hover the segments to get more details
including:

- Name
- Size in MiB
- Percentage of total
- Delta since baseline snapshot (if in diff mode)
- A brief explaination of the segment

The tree looks like this:

- File backed
  - Native libs (.so)
  - Other files
- Anonymous
  - Native
    - Seen by profiler
    - Allocator overhead (TODO what does this mean?)
  - Java
    - Reachable
    - Unreachable
  - Thread stacks
  - Other anon
- Other

#### Java Memory

Request java heap dumps

```
config: {
  name: 'android.java_hprof',
  targetBufferName: 'java_hprof',
  javaHprofConfig: {
    pid: [pid],
    continuousDumpConfig: {
      dumpIntervalMs: 10_000,
    },
  },
}
```

For the selected snapshot from the line chart above - find the nearest java heap
dump in the trace and display a breakdown of information.

##### Total size

Sum of the retained bytes from each of the objects in the heap dump.

##### Live Objects

Total count of all objects in the heap graph.

##### Registered Native

Sum of all the registered native memory in this snapshot - reachable only.

##### Art overhead

Derived from smaps - sums rss*kb from profiler_smaps for all mapping matching:
*.art* / *.oat* / *.odex* / *.vdex* / \_dalvik-jit*

##### Class breakdown tables

Three tables break the heap down by class. They are the **same per-class dataset
surfaced under three different sort orders** — identical columns, only the
ranking (and the share denominator, which tracks the ranking) differs.

**All three are reachable-only.** Unreachable objects (garbage awaiting GC) are
excluded entirely, so every column describes live memory and the columns are
comparable with each other — in particular `retained ≥ shallow` always holds.
This matches what mature heap tools do (Eclipse MAT and DevTools strip or
GC-away unreachable objects before reporting) and matches the in-tab
flamegraphs, which are reachable-only by construction (BFS from GC roots /
dominator tree). If we ever want to surface garbage / allocation churn, it
belongs in its own column or section explicitly labelled as unreachable — never
folded into these columns.

Vocabulary (standard heap-analysis terms — do not overload them):

- **Shallow**: an object's own bytes (`self_size`), Java heap only.
- **Retained**: dominator-tree retained size — everything freed if the instances
  went away (self + the subtree they _exclusively_ dominate + registered
  native). Reachable by definition. This is the only thing called "retained";
  the live self size is "shallow", not "retained".
- **Instances**: count of live instances of the class.

Common columns (every table, in this order):

- **Class** — class name, with the `↳ via <class>` retainer annotation (below).
- **Instances** — reachable instance count.
- **Shallow** — reachable self size (Java only).
- **Retained** — dominated size, Java + registered native. A class that owns big
  native buffers (bitmaps, NIO) shows up as `retained ≫ shallow`, so native
  doesn't need its own column.
- **Share** — this class's share of the reachable total of the column the table
  is ranked by, drawn as a tiny progress bar (retained-share for the retained
  table, shallow-share for the shallow table, count-share for the count table).

In diff mode every numeric column shows its delta vs the baseline dump.

The three tables:

- **Top classes by retained size** — ranked by retained (Java + native) desc.
  "What holds the most memory alive — the biggest wins if collected." This is
  the leak-hunting view.
- **Top classes by shallow size** — ranked by shallow desc. "Whose own instances
  occupy the most memory" — big arrays, bitmaps, buffers. A class can top this
  without topping retained (a large `byte[]` that holds nothing else) and
  vice-versa (a tiny map that dominates a huge subtree).
- **Top classes by instance count** — ranked by instance count desc. Surfaces
  churn from many small objects.

For library classes (`java.*`, `android.*`, `kotlin.*`, arrays, etc.) the class
cell shows a second `↳ via <class>` line - the nearest app-side class up the
dominator tree that retains them, i.e. the code in your app most responsible for
keeping them alive. When walking up finds no app-side owner before the GC roots,
it shows `GC root` instead. This annotation appears across all three tables. If
one of these sections is an aggregate of two different sets of instances which
are owned by multiple roots - we just choose the largest retainer.

> Note: because the columns are now identical and only the sort differs, these
> three tables could collapse into a single table with sortable column headers.
> Kept as three pre-sorted "top N" tables for now to suit the at-a-glance
> landing-page style with no interaction required.

#### Bitmaps

Displays information about reachable `android.graphics.Bitmap` instances in the
selected heap dump.

- Total bitmaps: A count of the reachable bitmap instances in this snapshot.
- Bitmap memory: Total bytes used by those bitmaps - Java self size + registered
  native, plus an estimate (width × height × 4) of the pixel buffer for non-heap
  (ashmem / hardware) backings, since those bytes aren't in self/native size.
  The sub-line splits it into heap vs ashmem (est).
- Largest group: The single dimension group (e.g. 1080×2400) consuming the most
  bytes - shows its total size and instance count. Only shown when the dump
  records bitmap dimensions (ART HPROF dumps; proto-format dumps don't).
- Of java retained: The bitmaps' heap self + registered native bytes as a
  percentage of the dump's total reachable Java heap + registered native.
  Excludes the estimated ashmem pixel bytes, since those aren't part of the Java
  retained total.

The insight callout also names the nearest app-side class retaining the bitmaps
(same dominator-walk as the `↳ via` annotation above), when resolvable.

When in diff mode, count, size and share show diffs. If this group didnt exist
in the baseline snap then show 'new'.

TODO: How's best to show who owns these bitmaps - seeing as they're grouped by
size.

#### Native allocations

Here we find the closest native allocations (not necesserily aligned to smaps /
java heap dumps) and drill into some stats about it.

##### Billboards

- RSS anon + swap: Total native allocator footprint from smaps (anonymous_kb +
  swap_kb of the native bucket - scudo/jemalloc/libc_malloc/GWP-ASan/[heap]) at
  the selected snapshot. This is the denominator for profiler coverage.
- Seen by profiler: Cumulative unreleased native bytes the profiler observed
  (allocations minus frees) up to the selected snapshot. The value is bytes; the
  sub-line shows it as a % coverage of the native RSS above, or the Δ vs
  baseline when comparing.
- Allocator overhead + unseen: RSS anon + swap − seen by profiler. The part of
  the native footprint the profiler can't explain: allocator metadata,
  fragmentation, and allocations made before tracing started.
- Thread stacks: Resident stack memory (anon + swap of [stack] / [anon:stack*])
  from smaps - resident, not the full reservation, since only touched pages
  count. Sub-line shows the thread count.

##### Profiler Coverage

A proportion bar showing the seen by profiler and unseen from above visually.

##### Table

A breakdown of unreleased memory seen by the profiler:

- Call-stack snippet: A mini callstack (regex based app only stack frames) back
  up to the root. Only the top 5 callsites by unreleased bytes are shown.
- Amount of unreleased memory from this callsite.
- Number of allocations.
- Share of profiled unreleased memory as a percentage.

When in diff mode - we don't show diffs, we just rescope the unreleased
calculation to run from the baseline to the selected snapshot (the column header
becomes "Δ unreleased").

### Smaps Detail Tab

The raw `/proc/<pid>/smaps` dump for one snapshot of the selected process,
folded into the same taxonomy as the composition chart. A dropdown picks which
snapshot to view (labelled `#N · t=Xs · <RSS>`, defaulting to the latest); it's
only shown when there's more than one. Unlike the overview tab this has its own
snapshot selection - it does not follow the line-chart selection - and has no
diff mode.

#### Smaps Billboards

Each is a sum over every mapping in the snapshot:

- Total RSS: Total resident set (rss_kb). Sub-line shows the mapping count.
- RSS anon + swap: Private anonymous + swapped (anonymous_kb + swap_kb).
- Total PSS: Proportional set size (proportional_resident_kb) - shared pages
  divided by the number of sharers, so the process's fair share.
- Private dirty: Private dirty bytes (private_dirty_kb) - the unshareable cost
  that can't be reclaimed without paging out.
- Swap: Bytes swapped out (swap_kb, e.g. zram).

#### Mapping Table

Every mapping in the snapshot, aggregated by path. Two view modes:

- Tree (default): mappings bucketed into a two-level taxonomy. Groups and
  sub-groups are collapsible and show their summed columns; leaf paths are
  listed under each sub-group (capped, with a "… N more" row).
- Flat: individual mappings sorted by RSS (capped at 200).

The taxonomy and the rule matching each sub-category (matched against the
mapping path):

- Anonymous (paths not starting with `/`)
  - Native heap:
    `^\[anon:(scudo|libc_malloc|jemalloc|GWP-ASan|partition_alloc|\.bss)` or
    exactly `[heap]`
  - Java heap: `^\[anon:dalvik-(main|large object|zygote|non moving|free list)`
  - Java other / ART: `dalvik` or `\.art\]$`
  - Thread stacks: `^\[stack\]` or `^\[anon:stack`
  - Other anon: anything else (fallback)
- File-backed (path starts with `/`)
  - Native libs (.so): `\.so$`
  - Java (.jar/.oat/.art): `\.(jar|dex|oat|odex|vdex|art)$`
  - Resources / APK: `\.(apk|ttf|otf|dat)$` or path starts with `/fonts/`
  - Other file-backed: fallback for any other `/…` path
- Graphics / shared
  - ashmem / dmabuf: `dmabuf` or `^/dev/ashmem`
  - GPU / driver: `^/dev/(kgsl|mali|dri)`

Classification is first-match in a fixed precedence - **graphics → file-backed →
anonymous specifics → other anon** - which is not the display order. The
graphics checks run first so `/dev/ashmem*` and `/dev/kgsl*` (which start with
`/`) land under Graphics rather than File-backed, and the dalvik heap spaces are
matched before the generic `dalvik` bucket.

A regex filter narrows the paths (falls back to substring match if the regex is
invalid), and an "All columns" toggle expands from the default RSS / Priv. dirty
/ Swap to also show PSS, Anon+swap, Priv. clean, and Shared dirty/clean.

## TODO List

- [x] Use Megabytes everywhere
- [x] What is shallow and retained?
- [ ] Fix graph scale y axis
- [ ] Think actionability - how to work out WHERE these allocations happens
  - [ ] Why do we not have callstacks for bytes and bitmaps in the java heap
        breakdown?
- [x] Go to the actual java heap dump when clicking show in timeline - don't
      just select the track.
- [ ] Shaded flamegraph regions showing dirty/clean split for file backed.
- [ ] Make it clearer when we're showing a diff.
- [ ] Fix the flamechart fatness when we have very thin sections.
- [x] How to switch between smaps snapshots in the smaps details tab.
- [ ] Might be nice to show WHICH GC root.
- [ ] Links to the flamegraph when clicking on a class in the java heap table.
- [ ] Detect whether the trace is obfuscated.
- [ ] Too bright red and green.
- [ ] There's a difference between the datasource not being configured and the
      data source not being there.
- [ ] Understand the config
- [ ] Little billboards - some clickable not availble or something.
- [ ] Clickable little chip in the BL of the line chart goes white when hovered.
- [ ] The top level trace overview - what to do if there are no snapshots.
  - [ ] Move the trace overview bar chart in the composition over time (and also
        show if we have more tha one snapshot) - and it should change depending
        on the diff.
- [ ] Remove the y axis for the line chart.
- [ ] Fix the 700B in the retained panel.
- [ ] If we click on java the flamegraph should we nav to other pages - on other
      tabs / pages.
- [ ] Degredation when SMAPs is not present.
- [ ] Make Heap reachability smaller
- [ ] Have unreachable heap as separate billboard along with reachable.
- [ ] The 'via' we should have multiple 'via' if that's the case.
- [ ] Should we have top down table as well?
- [ ] bitmaps - make them clickable and navigate to HDE.
- [ ] How does it look if we only have 1 snapshot - without smaps - without. -
      add bug for this.
- [ ] Link to heap dump explorer when clicking on class.
