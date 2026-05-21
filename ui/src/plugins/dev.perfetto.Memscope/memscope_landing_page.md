# Memory Overview Page · Spec

What the memory overview page shows when a recorded trace is opened, where every
number comes from, and the smells it surfaces.

> memory overview page · v2 · overview screen · web app · companion: Explainer
> doc

**Job of the memory overview page → Triage, not diagnosis.** Orient someone who
just opened a trace: how much memory, growing or steady, which way to dig
(managed vs native), and where to click. smaps owns the totals (OS truth); the
two profilers explain what's inside. Every element traces to a real measurement
or an honest "unknown."

## Summary

For a single selected process, the page shows three things:

1. **Charts**, one per data source, plotted over time:
   1. **smaps footprint** — anon+swap only, split into Java heap (dalvik),
      Native heap (malloc allocator arenas) and Other.
   2. **Native heap (heapprofd)** — cumulative allocated, freed, and unreleased
      (net) over time.
   3. **Java heap (java_hprof)** — total reachable bytes by class (top 6 +
      Other) + unreachable (aggregated).
2. **Insights** — a small set of automatic observations, each flagged only when
   active and always showing the numbers behind it:
   1. Process footprint growing (anon+swap, smaps).
   2. Unreleased native memory growing (heapprofd).
   3. Java heap growing across dumps.
3. **Snapshot table** — every dump, smaps sample, and native profile listed in
   time order, with links to the timeline and (for dumps) the Heap Dump
   Explorer.

When a source is missing for the selected process, its insight is shown as a
neutral placeholder ("this process has no …") rather than hidden.

What it deliberately does **not** do:

1. **Combine or correlate sources.** Each datasource is represented on its own
   chart; nothing is overlaid on a shared graph and no attempt is made to tie
   one datasource type to another. However, smaps provides a robust overview
   chart.
2. **Detect problems reliably.** The insight rules are intentionally simple
   first-vs-last-sample comparisons. They produce false positives — memory can
   be legitimately higher at the end of a trace — but they're easy to understand
   and act as a starting point we can interate on.
3. **Cover non-malloc native allocators.** We don't distingush the art allocator
   if also collected by heapprofd. We assume all heap profile dumps are native.
4. **Present the most useful breakdowns in the top level charts** The axis for
   breaking down the various data sources is somewhat arbitrary:
   1. Smaps: java/native/other seems sensible, but we could go further.
   2. heapprofd: allocated/released/unreleased seems sensible - we could break
      down by other metrics e.g. top X callsites?
   3. Java Heap Dumps: by top X classes + reachable seems sensible - could also
      breakdown by JNI root perhaps.
5. There's a lot more interesting things we could do with smaps dumps that I'm
   currently not doing. E.g. clean/dirty, file RSS, various breakdowns by
   different categories of paths.

Known issues:

- Page performance is bad.
- Page generally lacks polish.
- Gives no guidance on which tracing options to enable when a datasource is
  absent.
- Smaps are reported using a different upid to the parent process they're
  recording so some smaps snapshots are associated with process `<unknown>` or
  `init`. Fixed by
  https://googleplex-android-review.git.corp.google.com/c/platform/art/+/40226332.
- For smaps you'll need to be running >ZP1A.260602.001

## 0 · Inputs

Three sources, three cadences, sharing only the wall-clock axis.

| Source         | Description                                                                                                               |
| -------------- | ------------------------------------------------------------------------------------------------------------------------- |
| **smaps**      | Absolute per-mapping memory, sampled periodically. The truth. Total footprint & the region breakdown.                     |
| **java_hprof** | Absolute managed-object state (ART heap dump), sampled periodically. Per-class counts & bytes. No object values.          |
| **heapprofd**  | Net (alloc−free) by callsite since record start, sampled periodically. Per-interval allocated & freed bytes. malloc only. |

Don't assume a fixed cadence — take whatever sample intervals the trace actually
has and plot each source at its own real timestamps. Never imply the values were
sampled at one instant.

## 1 · Layout

Top to bottom: identity → capture strip → process selector → three charts (the
evidence) → problems (the smells) → snapshot table.

- **Section header** — "Memory Overview" title with subtitle: "Memory triage:
  smaps owns the total, the native and Java profilers explain what is inside."
- **Capture strip** — one row per data source with a colored dot + terse facts:
  - 🟢 **smaps**: sample count + time span
  - 🔵 **java_hprof**: dump count
  - 🟠 **heapprofd**: sample count + time span
  - Shows selected process name, trace title, and trace duration.
- **Process selector** — dropdown to switch between processes (shown only when
  multiple processes have data). Default selection is the process with the most
  data, weighted: heap dumps (3×) > smaps (2×) > native profiles (1×).
- **Chart 1** — Process footprint (smaps). Anonymous + swap over time, split
  into Java Heap, Native Heap and Other.
- **Chart 2** — Native allocator (heapprofd). Three lines: Allocated, Released,
  and Unreleased (net) over time.
- **Chart 3** — Java heap composition (java_hprof). Stacked area by class over
  time.
- **Problems** — smell cards (see §3).
- **Snapshot table** — chronological list of all heap dumps, smaps samples, and
  native profiles with timestamps, types, sizes, and drill-down actions.

## 2 · The charts

### Chart 1 · Process footprint — source: smaps

- **Type:** Stacked area over time.
- **Data:** `anonAndSwap` = Σ (`anonymous_kb` + `swap_kb`) across all smaps
  mappings, split into three bands by mapping name:
  - 🟩 **Java Heap** — dalvik object spaces (`*dalvik*`, `/dev/ashmem/dalvik*`)
  - 🟦 **Native Heap** — malloc allocator arenas (`[anon:scudo*`,
    `[anon:libc_malloc*`, `[anon:jemalloc*`, `[anon:GWP-ASan*`, `[heap]`)
  - 🟨 **Other** — everything else (thread stacks, graphics/DMA, mmap'd regions,
    unnamed `[anon]`)
- **Condition:** Shown only when there are ≥ 2 smaps samples.
- **Defense:** anon+swap = memory we own that can't be freed without swapping;
  it deliberately excludes file-backed (reclaimable) pages.
- **Caveat:** "Native Heap" is the allocator-arena subset only — other native
  memory (stacks, graphics, mmap, unnamed anon) lands in "Other", which is
  shown, not hidden. The band is a best-effort attribution by mapping name; it
  is not derived by subtracting heapprofd.

### Chart 2 · Native allocator — source: heapprofd

- **Type:** Stacked area over time (two bands).
- **Bands** (sum to total allocated, by construction):
  - **Unreleased** (bottom) — cumulative net outstanding (allocated − freed);
    the live malloc memory.
  - **Released** (top) — cumulative freed (allocated − unreleased).
- **Why stacked:** Unreleased + Released = total ever allocated, so the stack
  height is "Allocated" and the split shows live vs freed without a third line.
- **Reads as:** Growing Unreleased band = leak signal. Thick Released band (lots
  cycled through) = churn / fragmentation.
- **Note:** the bands are not independent — Released is derived as
  `allocated − unreleased` from the signed `size` rows in
  `heap_profile_allocation` (positive = alloc, negative = free).
- **Scope:** malloc only.

### Chart 3 · Java heap composition — source: java_hprof

- **Type:** Stacked area over time, split by reachability then class.
- **Bands** (bottom → top):
  - **Reachable, top 6 classes** — the 6 classes with the highest peak reachable
    bytes, each its own band (class name trimmed to last segment, e.g.
    `java.util.HashMap$Node` → `HashMap$Node`).
  - **Other (reachable)** — all remaining reachable classes.
  - **Unreachable** — every unreachable object (uncollected garbage), collapsed
    into one band, drawn on top.
- **Why by reachability:** separates live retention (reachable, the leak
  surface) from garbage awaiting collection (unreachable). The class split is
  applied only to reachable bytes — that's the dimension worth attributing.
- **Why by type:** class is the only dimension stable across dumps (object
  addresses change; class totals are comparable).
- **Ranking:** top-N is by peak _reachable_ bytes, so the bands reflect the live
  composition.

## 3 · Insights (the smells)

Rendered as the **"Insights"** panel. Three cards, in this order, each tied to
one data source and always showing the numbers behind it. Each card compares the
first and last sample of its series.

1. **Total footprint** — anon+swap grew or shrank over the sampled window.
   _smaps · "Anon + swap grew by X in Y seconds."_

2. **Unreleased native memory** — net outstanding (allocated − freed) grew or
   shrank. _heapprofd · "Native unreleased grew by X (from A to B)."_

3. **Java heap growth** — total dump bytes grew or shrank across dumps.
   _java_hprof · "Heap total grew by X (from A to B)."_

**Card states** (icon via the `Icon` widget):

- ⚠️ **info** (warning intent) — the smell is active (growing).
- ✓ **check_circle** (success intent) — checked and healthy (stable or
  shrinking).
- ⊝ **remove_circle_outline** (neutral, muted) — the data needed for this check
  isn't present; the card reads "This process has no … " rather than being
  hidden.

**Runner-up structural smells** (later, all value-free): oversized/empty
collections (array length vs element count), dominator heavy-hitters (one object
retaining a huge subtree), leaked Activity/Context (root→destroyed-lifecycle
reference chain).

## 4 · Snapshot table

Chronological table of all memory measurement snapshots for the selected
process.

| Column                | Description                                                                     |
| --------------------- | ------------------------------------------------------------------------------- |
| Time                  | Timestamp of the sample                                                         |
| Type                  | Chip: "java_hprof" (primary), "smaps" (none), "heapprofd" (success)             |
| Heap                  | Heap name (heapprofd rows only)                                                 |
| Smaps (anon+swap)     | smaps rows: total anon+swap bytes                                               |
| Java heap (reachable) | java_hprof rows: reachable bytes (absolute) + Δ vs the previous dump            |
| Native heap Δ         | heapprofd rows: Δ unreleased vs previous sample; sample count below             |
| (actions)             | "View on timeline" button; "Open in Heap Dump Explorer" button (for dumps only) |

Each row populates only the size column for its own source; the other size
columns are blank. The sample count lives under the Native heap Δ value (no
separate column). Both actions render as buttons with the same styling. "View on
timeline" selects the relevant track/event and navigates to the viewer; "Open in
Heap Dump Explorer" navigates to `#!/heapdump?upid=…&ts=…`.

## 5 · What the memory overview page explicitly does not claim

- It is **not** total RSS — file-backed pages are excluded on purpose.
- The breakdown is best-effort attribution; the unexplained remainder is shown,
  not hidden.
- The native graph is malloc-scoped, not all allocators.
- No duplicate / boxing analysis — the dump has no object values.
- Auto-verdicts (if any) are heuristic and must show their evidence, not just
  assert.

## 6 · Open questions

- Is there a hard limit/ceiling to draw on Chart 1 (cgroup, lmkd threshold,
  `-Xmx`)? Changes the "how big" verdict.
- Thresholds for each smell — what slope/magnitude flips "fine" → "suspect" →
  "problem"?
- Is the memory overview page triage (find the problem) or health dashboard
  (reassure)? Affects "smells" vs "checks" framing.
- Does the native source ever capture allocators beyond malloc (scudo hooks)?
  Affects Chart 3 scope copy.

---

**One-line summary:** smaps gives the honest total (anon+swap) and its region
breakdown; the native profiler and Java dump explain what's inside their regions
over time; three smells — footprint growth, unreleased native growth, and
growing Java heap — point the user at the right drill-down. Total is truth,
breakdown is best-effort, unknown is shown.
