# Unifying machine, GPU, and custom track dimensions

**Authors:** @dreveman

**Status:** Draft

**PR:** N/A

## Problem

Perfetto has grown several independent mechanisms that do fundamentally the same
thing: take an identifier attached to a set of tracks and, *only when more than
one distinct value is present*, surface it in the UI — either as a name suffix or
as an extra level of track hierarchy. Each is hand-rolled, and each bakes in a
different, fixed choice of how it appears:

- **Machine.** In a multi-machine trace, thread/process/CPU/GPU tracks get a
  ` (machine N)` (or ` (<name>)`) suffix — a *label*, no dedicated tree node.
  Implemented by a shared machine-label helper, a dense per-machine index
  (`label_index`) in the prelude `machine` view, and applied at ~10 track-naming
  call sites.

- **GPU.** In a multi-GPU trace, GPU tracks get an *extra hierarchy level* plus a
  `GPU N` / name label. Implemented in `dev.perfetto.Gpu`, with a separate,
  re-implemented per-process gate in `dev.perfetto.GpuByProcess`. The GPU code
  already reuses the machine-label helper and folds machine into its sort order.

- **Custom, workload-defined dimensions (the gap).** There is real demand for
  another identifier that is generic and specific to the workload — declared by
  the producer rather than derived by the trace processor. The motivating example
  is the **rank** of a PyTorch distributed-training process, but "shard",
  "replica", "worker", "stage" are all the same shape. There is no way for a
  producer to declare such a dimension today, and adding one the current way would
  be a *third* hand-rolled copy of the same collapse-and-label logic.

The data model is already partly generic — arbitrary track dimensions live in the
track's dimension arg set and are read via `extract_arg` — but there is no
producer surface for a generic labeled dimension (`TrackDescriptor` only has typed
`process`/`thread`/`counter`/`state`), and no shared labeling layer above
dimensions.

We define one concept — a track **dimension**, in trace_processor's existing
sense — that machine, GPU, process/thread, and custom identifiers are all
instances of, and surface it through a single labeling layer. The goal is:

1. Adopt trace_processor's **dimension** terminology as the single vocabulary for
   machine, GPU, process/thread, and custom identifiers.
2. Give producers a first-class surface to declare **custom dimensions**.
3. Expose every dimension through one typed, queryable trace_processor surface,
   independent of whether it came from a typed descriptor, import context, or a
   producer-declared custom value.
4. Turn collapse-and-label behavior into **shared presentation helpers** — so
   machine and custom dimensions use them directly, GPU's existing hierarchy
   consumes the same label metadata, and details panels show the same values.

A dimension is first a trace_processor/query concept. Track subtitles are its
normal timeline presentation, but not its only surface: dimensions are also
available to SQL, details panels, and specialized UI. A well-known dimension can
have specialized presentation — GPU hierarchy today — without becoming a
special data model. Hierarchy for system-wide concepts comes from
trace_processor identity/merging, not a generic UI grouping mode (see
Well-known identity vs producer-local structure and Alternatives).

Throughout this RFC, **"custom dimension"** means a producer-declared,
workload-specific dimension; **rank** is used only as a concrete example of one.

## Decision

Adopt `Dimension` / `TrackDescriptor.dimensions` as a producer surface and
normalize custom and well-known dimensions into one typed, track-keyed
trace_processor relation. Dimensions declared on process/thread tracks resolve
through existing `upid`/`utid` association; dimensions on ordinary tracks resolve
through `parent_uuid`. Conflicting child overrides are invalid. The standard UI
presentation is a multi-label track subtitle, details tabs show the same resolved
values, and specialized consumers such as GPU hierarchy use the same TP data and
label helpers. The initial well-known set is `machine`, `gpu`, `cpu`, `process`,
and `thread`; custom string data is interned; numbering is per source trace and
machine.

## Design

### Terminology: dimensions

We align on trace_processor's existing concept. A **dimension** is a named,
typed key/value in a track's identity. There is one vocabulary and one resolved
trace_processor representation regardless of whether the value originated in a
typed descriptor, import context, or `TrackDescriptor.dimensions`.

Dimensions fall into two categories:

- **Well-known dimensions** — recognized by trace_processor and shared across data
  sources: initially `machine`, `gpu`, `cpu`, `process`, and `thread`. Their
  defining property is that the same canonical value in different data sources
  refers to the same real thing. This permits validation, merging, and specialized
  presentation. The set is declared centrally; custom producers cannot redefine
  these reserved names.
- **Custom dimensions** — producer-declared and workload-specific (`rank`,
  `shard`, `stage`, …). Their identity is local to the producer/source trace and
  they are not cross-data-source merge keys.

A dimension declaration carries:

- **name** — the canonical dimension key (`rank`, `shard`, …).
- **typed value** — initially an integer or an interned string.
- **display name** — an optional interned per-value label such as `worker-east`.

A dimension has no independent `scope` enum. It is declared on a track. Existing
process/thread association and explicit `parent_uuid` relationships determine
which other tracks resolve that declaration as part of their effective dimension
set. “Process-scoped rank” is shorthand for “rank declared on the process track,”
not a separate storage or importer concept.

Presentation metadata adds a label template (`machine %d`, `GPU %d`, `rank %d`)
and stable numbering. Numbering is deterministic and gap-free within a
`(source trace, machine, dimension name)` partition. A producer-supplied
`display_name` overrides only the rendered numbered label; it does not replace
the canonical typed value used by SQL or merging.

### Well-known identity vs producer-local structure

There is a useful distinction in how a producer's track event relates to the rest
of the trace. It shapes how dimensions participate in identity and presentation:

- **(a) Intersects a system-wide concept — needs merging.** The dimension names
  something the trace already knows about globally, so track-event tracks want to
  *merge* with other data sources that carry the same dimension. GPU is the
  example: GPU track-event tracks should sit alongside GPU counter-descriptor
  tracks for the same GPU. Thread/process are the same shape. This merging is a
  **trace_processor** responsibility, keyed on well-known dimensions — and it is
  what produces GPU hierarchy today.
- **(b) Standalone.** The dimension carries no system-wide meaning; the producer
  owns the structure entirely via `TrackDescriptor.parent_uuid`. Custom dimensions
  are here. Grouping is the producer's; we only *label*.

Hierarchy that comes from merging system-wide concepts belongs in trace_processor,
not in a UI mode layered over arbitrary producer trees. This RFC builds the
shared producer and trace_processor dimension model plus its subtitle/details
presentation. Existing GPU hierarchy remains a specialized UI consumer of the
well-known `gpu` dimension. Generalizing identity merging to more well-known
dimensions remains future work.

### Presentation: subtitles, details, and specialized UI

The normal timeline presentation of a dimension is a **label**: a secondary
annotation on the affected node, not a mutation of its canonical name. This RFC
adds a first-class track/group **subtitle** affordance (secondary text under the
name), modeled on Chrome's existing behavior. It supports multiple labels in a
stable order, separated by `·`, with truncation and the complete values available
through tooltip/accessibility text.

The shared collapse rule is unchanged: if the applicable tracks have a single
distinct value for a dimension, its subtitle label is invisible; if they have
more than one, the label is shown. Dimensions collapse independently. Labeling
never reparents tracks.

A process `trainer` on machine 1 with custom `rank 3` in a multi-machine,
multi-rank trace:

```text
workspace
└── trainer                        (process group; name unchanged)
    ⤷ machine 1 · rank 3           (subtitle labels)
    └── <threads / tracks>
```

The same effective dimensions are shown in details tabs as canonical name, raw
typed value, and optional display name. SQL exposes them independently of UI
presentation.

**No generic LEVEL mode.** GPU hierarchy remains specialized presentation in
`dev.perfetto.Gpu` / `dev.perfetto.GpuByProcess`. At the trace_processor level,
`gpu` is indistinguishable in shape from another dimension. The GPU UI consumes
the shared dimension metadata and label helper, applies the resulting `GPU N` /
name as its existing group-node title, and keeps its hierarchy behavior:

```text
GPU
└── GPU 0                          (specialized presentation of gpu dimension)
    ⤷ machine 1                    (shared subtitle label)
    └── <gpu tracks>
```

### Resolution through `parent_uuid` and process/thread association

Labeling never reparents anything, but effective dimension resolution is explicit
and shared by SQL, subtitles, details, and specialized UI:

1. A dimension declared on a process descriptor applies to tracks associated with
   the same `upid`, including its threads, process tracks, and per-process GPU
   tracks.
2. A dimension declared on a thread descriptor applies to tracks associated with
   the same `utid`.
3. A dimension declared on any other track applies to that track and its explicit
   `parent_uuid` descendants.
4. A global track with no process/thread association receives only dimensions
   declared on itself or its explicit ancestors, plus synthesized well-known
   dimensions such as machine.
5. A descendant can add a dimension name not provided by its effective ancestors.
   Repeating the same name and typed value is redundant and deduplicated. A
   different value for an inherited name is invalid producer input: trace_processor
   records an import error and does not apply the override.

For example:

```text
Process track: process=trainer, rank=3
├── Thread track                         effective: rank=3
├── CPU annotation track                effective: rank=3
└── Per-process GPU track, gpu=1         effective: rank=3, gpu=1
```

But:

```text
Process track: rank=3
└── Child track: rank=4                  ERROR: inherited value override
```

Sibling subtrees may carry different values when their common parent did not
declare that dimension. This keeps producer-defined grouping in `parent_uuid`
while making inheritance deterministic and preventing a child from silently
breaking the identity model.

### Data model and pipeline

1. **Producer surface (new) — `TrackDescriptor`.** Add repeated custom
   dimensions to `TrackDescriptor` (field numbers are illustrative until the
   proto change is prepared):

   ```proto
   message TrackDescriptor {
     // ... existing fields (uuid, parent_uuid, name, process, thread, counter,
     // state, ordering, ...) ...

     // Custom dimensions declared on this track. Repeated so a track can carry
     // more than one independent dimension.
     repeated Dimension dimensions = 21;
   }

   message Dimension {
     optional uint64 name_iid = 1;           // e.g. interned "rank"
     oneof value {
       int64 int_value = 2;
       uint64 string_value_iid = 3;
     }
     optional uint64 display_name_iid = 4;   // e.g. interned "worker-east"
   }
   ```

   Dimension names, string values, and display names are interned in the
   packet-sequence incremental state. The exact `InternedData` entry or shared
   interned-string namespace is an implementation detail to settle with the
   proto change. IIDs are sequence-local transport details: trace_processor
   resolves them immediately to canonical typed values, and unknown IIDs produce
   an import diagnostic. Integer values remain inline.

   The message intentionally contains no scope. The dimension is declared on
   the track described by the containing `TrackDescriptor`; the resolution rules
   above determine its effective descendants. Producers use typed descriptors
   for well-known dimensions and cannot declare a custom dimension with a
   reserved well-known name.

2. **trace_processor import and resolution.** All dimensions use one generic
   track-dimension path. Typed process/thread descriptors, GPU descriptors, and
   machine import context synthesize the same canonical rows as custom
   declarations. A post-import resolver applies process/thread association and
   `parent_id` inheritance, deduplicates repeated identical values, and reports a
   producer error for a conflicting inherited value. There are no separate
   custom process-, thread-, or GPU-dimension storage paths.

3. **trace_processor query surface.** Expose effective dimensions through one
   typed long-form relation (public name to be finalized):

   ```text
   track_dimension(
     track_id,
     name,
     value_type,
     int_value,
     string_value,
     display_name,
     declaring_track_id,
     is_inherited,
     is_well_known
   )
   ```

   A small registry exposes dimension metadata such as the well-known bit and
   label template. The generic relation is keyed by `track_id`, so slices,
   counters, GPU work, and other track-backed domains use the same join. Raw
   IIDs never escape into SQL.

4. **Stable numbering.** Generalize the existing dense machine index into a
   deterministic index per `(source trace, machine, dimension name)`. Values are
   ordered by canonical type and value rather than packet arrival. Tracks with
   no machine use a source-trace-global synthetic machine bucket. Merged input
   traces retain their source-trace identity for numbering unless future merging
   explicitly unifies that dimension.

5. **UI.** Factor presentation into: (i) a label helper that maps a canonical
   value to its template/numbering/display override; (ii) a generic collapse pass
   that emits subtitle labels; and (iii) a details renderer for all effective
   dimensions. Machine and custom dimensions use all three. GPU uses the same TP
   rows and label helper but retains its specialized group presentation.

### Custom dimensions and GPU work

GPU is a well-known dimension with the same trace_processor shape as machine,
CPU, process, thread, or a custom dimension. Its special behavior is limited to
identity merging and UI presentation.

A per-process GPU track is associated with a `upid`, so a custom dimension
attached to that process track — for example `rank` — resolves onto the GPU track
through the ordinary process-association rule. No GPU-specific custom-dimension
code or `_process_dimension` join is needed. A cross-process GPU group does not
receive a process dimension when its children disagree: `rank` remains on the
per-process child tracks while the group carries only dimensions that have one
unambiguous value, such as `gpu` and `machine`.

### Querying by a dimension in trace_processor

Dimensions are a first-class query axis for ad-hoc SQL and batch analysis. The
long-form `track_dimension` relation preserves value types, so joins do not rely
on string coercion:

```sql
-- Any slices on tracks whose effective rank is integer 3.
SELECT s.*
FROM slice AS s
JOIN track_dimension AS d USING (track_id)
WHERE d.name = 'rank' AND d.int_value = 3;

-- GPU busy time per rank. GPU work uses the same track_id relation.
SELECT d.int_value AS rank, sum(s.dur) AS gpu_busy
FROM gpu_slice AS s
JOIN track_dimension AS d USING (track_id)
WHERE d.name = 'rank'
GROUP BY rank;
```

Process and thread remain typed top-level Perfetto concepts. Their canonical
tracks declare or synthesize dimensions, and the resolver propagates those
values to tracks with matching `upid`/`utid`; they do not require parallel
per-scope dimension tables. Machine continues to reach global tracks because TP
synthesizes it from each track's import context. A global track receives a custom
dimension only from its own descriptor or an explicit `parent_uuid` ancestor.

### Details and non-track surfaces

When a selected slice, counter, or other row has a backing `track_id`, the details
panel resolves and displays that track's effective dimensions. Process/thread
details use their canonical tracks. Each entry shows the canonical name, raw
typed value, and optional display name; well-known dimensions such as machine no
longer need a separate raw-ID-only presentation. This avoids copying custom
columns into every event table while making dimensions consistently visible in
both SQL and details UI.

### Migration

- **Machine** → a well-known dimension synthesized on every track. Its timeline
  label moves from a name suffix to the new subtitle immediately; the machine
  table and canonical track names remain.
- **GPU** → a well-known dimension in the generic TP relation. Existing GPU
  hierarchy and the hardcoded multi-GPU presentation gate remain, but both GPU
  plugins consume the shared dimension metadata and label helper.
- **Process/thread** → remain typed top-level concepts and provide association
  edges for effective-dimension resolution. This RFC does not replace `upid` or
  `utid` storage.
- **Custom** → a producer declares a dimension on the appropriate process,
  thread, or ordinary track. Adding another custom dimension is then data rather
  than new TP/UI code.

Labels still collapse when only one distinct value is present. The deliberate
presentation change is that machine labels move from name suffixes to subtitles.

## Alternatives considered

### Option 1 — Generic TP dimensions + shared presentation; GPU hierarchy untouched (recommended)

Adopt trace_processor's dimension vocabulary, add the producer surface for custom
dimensions, resolve all effective values into one typed track-keyed relation, and
extract subtitle/details/label helpers shared by machine, custom dimensions, and
GPU presentation. Do **not** add a generic hierarchy mode; leave GPU's existing
merging-based grouping as specialized presentation.

Pro:

- One query model for custom, process/thread-associated, machine, CPU, and GPU
  dimensions; no per-domain custom-dimension tables.
- One implementation of collapse-and-label; the custom case is data.
- No generic LEVEL semantics, so no risky interaction with producer
  `parent_uuid` trees.
- Producers get a first-class way to express workload identity; labels are
  decoupled from canonical names and details UI reads the same source.

Con:

- Does not yet unify GPU-style hierarchy under a single presentation mechanism —
  GPU grouping stays specialized until merging work is generalized.
- Effective-dimension resolution and inheritance validation add TP work.
- Subtitle rendering is a new UI surface, and moving machine off its name suffix
  needs a diff-test sweep.

### Option 2 — Generic LEVEL presentation mode

Model machine and GPU as instances of one grouping concept with LABEL/LEVEL modes,
and drive GPU's hierarchy through a generic UI grouping pass.

Pro:

- A single mechanism spans labels and hierarchy.

Con:

- LEVEL semantics interact badly with the track-event surface and producer
  `parent_uuid` subtrees. Hierarchy from system-wide concepts belongs in
  trace_processor merging, not a UI mode.
- Re-expressing GPU risks subtle diffs in existing names/ordering.

### Option 3 — Add each custom dimension the current way (do nothing generic)

Give a custom dimension (e.g. rank) its own dimension and its own hardcoded
collapse/label gate, like GPU got.

Pro:

- Minimal and self-contained; ships fastest.

Con:

- A third hand-rolled copy — exactly what this RFC exists to avoid, and it
  guarantees a fourth.

## Future work (non-goals of this RFC)

- **Merging well-known dimensions in trace_processor.** Extend the mechanism that
  already merges process/thread across data sources to the initial well-known set
  (`machine`, `gpu`, `cpu`, `process`, `thread`), so hierarchy for system-wide
  concepts falls out of trace_processor identity rather than any UI grouping mode.
  This is the path to eventually folding GPU's specialized grouping into a shared
  presentation. Replacing the current typed process/thread storage with the generic
  identity model is not required by this RFC.
- **User-configurable presentation.** Letting the user change how a dimension is
  surfaced (e.g. promote a label to its own subtree) and reorder dimensions at
  runtime.

## Remaining implementation questions

The review resolves the model-level questions: use `Dimension` / `dimensions`,
add subtitles now, reserve `machine`/`gpu`/`cpu`/`process`/`thread`, reject
conflicting inherited values, intern string data, number per source trace and
machine, and expose dimensions in SQL and details UI. Implementation still needs
to settle:

- **Interned-data layout.** Use dedicated dimension-name/value/display-name IID
  namespaces or an existing generic interned-string entry; define incremental
  state reset behavior and validation for unknown IIDs.
- **Public query names.** Finalize the public table/view and registry names, and
  whether `declaring_track_id` / inheritance provenance are public or internal.
- **Import error behavior.** A conflicting child never overrides its inherited
  value. Follow TP conventions to decide whether to reject only the declaration
  or packet while recording the diagnostic; do not fail the whole trace merely
  because one producer emitted an invalid dimension.
- **Source-trace numbering key.** Define how archive/manifest imports persist a
  component source-trace identifier used with `machine_id` for stable numbering.
- **Subtitle layout.** Finalize height, truncation, styling, and accessibility for
  multiple labels; this is UI implementation detail rather than an optional
  feature.
