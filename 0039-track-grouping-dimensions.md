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
4. Provide **source-independent shared presentation** from that query surface.
   The first UI revision labels custom dimensions with existing track chips;
   migration of specialized well-known presentation is a separate phase.

A dimension is first a trace_processor/query concept. Custom chips are the
initial timeline presentation, but not the only surface: dimensions are also
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
trace_processor relation. Custom dimensions apply to the declaring track and
its `parent_uuid` descendants; process/thread association is not inheritance.
Conflicting inherited overrides are invalid.

The initial UI presents custom dimensions as **chips** in the existing track
shell. One trace-scoped index consumes the TP relation, independently of which
plugin or importer supplied the tracks. Labels are derived at render time from
registered track metadata, not stored as another workspace annotation system.
Chip participation is explicit: TrackEvent and TraceProcessorTrack provide
sources by default; other plugins opt in case by case using the same mechanism.
Details read TP's display values while canonical typed values remain available
for queries and identity.

Well-known dimensions retain their existing specialized timeline presentation
in this first revision. A later migration must specify specialized-handler
ownership before changing machine suffixes or GPU hierarchy labels. Generic
numbering and label templates are also follow-up presentation work.

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

A dimension has no independent `scope` enum. It is declared on a track, and
explicit `parent_uuid` relationships alone determine which other tracks resolve
that declaration as part of their effective dimension set. “Process-scoped rank”
is shorthand for “rank declared on the process track,” not a separate storage or
importer concept.

The first custom chips use the producer value or optional `display_name`; the
label does not replace canonical identity. Generic label templates and stable
numbering are future presentation work, with deterministic numbering per
`(source trace, machine, dimension name)` partition rather than packet arrival.

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
shared producer and trace_processor dimension model plus its chips/details
presentation. Existing GPU hierarchy remains a specialized UI consumer of the
well-known `gpu` dimension and is not changed by the initial custom-chip UI.
Generalizing identity merging and shared well-known presentation remains future
work.

### Presentation: chips, details, and specialized UI

Custom dimension labels use the existing `TrackNode.chips` presentation surface:
inline annotations beside the title, visible on normal short tracks without
adding a second subtitle row or changing track heights. Existing titles,
subtitles (including Chrome process labels), and static status chips remain
unchanged. Chips consume horizontal space; ordering, truncation, tooltip and
accessibility behavior use the existing track-shell affordances.

A custom dimension name is labelled when it has more than one distinct **typed
canonical value** across the trace. Dimensions collapse independently. This is
the initial approximation of "applicable tracks"; it does not depend on whether
values are integers or strings. Integer 3 and string "3" are distinct identities.

For example, a process group may show a custom rank chip next to its existing
name, including an unchanged machine suffix:

```text
workspace
└── trainer (machine 1)    [rank 3]
    └── worker            [stage forward]
```

The rank chip is omitted on a descendant which has the same typed value as a
visible ancestor. Workspace ancestry is used only to suppress redundant labels;
it does not create a TP inheritance edge. Hidden headless ancestors do not
suppress a visible child's chip.

Details show the same effective dimensions, including dimensions collapsed from
the timeline, using `display_value` and optional `display_name`. SQL retains the
canonical integer/string value for joins; well-known display values resolve
internal IDs to CPU/GPU numbers, PIDs/TIDs and machine label indexes.

**No generic LEVEL mode.** Existing machine suffixes and GPU hierarchy/naming
remain specialized presentation in this revision. Sharing their labels through
the generic mechanism is a follow-up, not a permanent name list in core.

### UI architecture and workspace state

1. **One trace-scoped index.** Core constructs an internal `TrackDimensionIndex`
   after EOF and initializes it once before activating the trace and loading
   plugins. It queries custom rows from `track_dimension`, with lossless int64
   values and typed comparison keys. A required schema/query/decode failure
   rejects trace loading and disposes the unactivated trace; it is not silently
   treated as an empty index. The HTTP version-compatibility flow remains in
   charge of using a compatible processor/UI. Optional details sections keep
   their own failure containment.
2. **Explicit plugin participation.** During rendering, `TrackView` resolves
   each node's URI to its registered track. `tags.dimensionSourceTrackIds` opts
   that track into chips using its TP dimension sources; absent or empty sources
   mean no chips. TrackEvent and TraceProcessorTrack set this to their event
   `tags.trackIds` by default. Other plugins opt in case by case; a virtual or
   summary track can supply different semantic sources. Core does not select
   plugins by ID. Event lookup, flows, selection and navigation continue to use
   `tags.trackIds`, whose presence alone does not enable dimension presentation.
3. **Explicit group sources.** Group creators own the semantic association.
   The process-summary plugin obtains the process-root TP track IDs in its
   existing metadata query and registers those sources. It does not union
   descendants or infer dimensions from `thread.upid`. Other groups can supply
   their own sources without a process-specific rule in the resolver.
4. **Conservative aggregation.** Deduplicate source IDs, intersect dimension
   names across every represented source, and show a value only when all sources
   agree on typed identity and rendered label. Missing or mixed values are
   omitted in V1; the UI never selects an arbitrary first value. Surviving
   dimensions are ordered by name, then redundant visible-ancestor labels are
   removed.
5. **Derived, non-persisted chips.** Rendering merges dimension chips with the
   node's existing chips without mutating the node. `TrackNode.subtitle` stays
   a string and `TrackNode.chips` stays its existing string array. No keyed
   subtitle map or contributor state is introduced. Clone, pin, custom workspace
   and JSON restore preserve static annotations; dimension chips are recomputed
   from the current URI/registry, so restoration ordering needs no replay hook.
6. **Staged specialized policy.** V1 labels only custom dimensions; it has no
   hard-coded `PRESENTED_ELSEWHERE` name list. A later well-known migration must
   agree how specialized plugins claim presentation and how unclaimed values
   are labelled. Core must not infer that policy from names or plugin IDs.

Presentation implementations remain internal to the UI. The public surface is
limited to semantic source-ID metadata, not formatting functions or plugin
presentation policy. No SQL query runs during rendering and no plugin performs
its own dimension-label scan.

### Resolution through `parent_uuid`

Labeling never reparents anything, but effective dimension resolution is explicit
and shared by SQL, chips, details, and specialized UI:

1. A dimension declared on a track applies to that track and its explicit
   `parent_uuid` descendants, whether it is a process, thread, or any other
   track.
2. Sharing a `upid` or `utid` with the declaring track is not an inheritance
   edge. Threads and other process-associated tracks do not receive a process
   dimension merely through that association.
3. A descendant can add a dimension name not provided by its effective ancestors.
   Repeating the same name and typed value is redundant and deduplicated. A
   different value for an inherited name is invalid producer input: trace_processor
   records an import error and does not apply the override.

Every track also receives the synthesized well-known dimensions, such as machine.

For example:

```text
Process track: process=trainer, rank=3
├── CPU annotation track (parent_uuid)   effective: rank=3
└── GPU track (parent_uuid), gpu=1       effective: rank=3, gpu=1

Thread track: rank=3 declared directly   effective: rank=3
Thread track with no dimensions          effective: (no custom dimensions)
mem.rss of trainer (process stats)       effective: (no custom dimensions)
```

Process association is deliberately not an inheritance edge. Producers which want
process dimensions on their thread tracks should re-emit those dimensions in the
thread descriptors. Queries which want "everything in rank 3, including its
threads" join through `thread.upid` to the process track explicitly (see Querying
by a dimension).

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
   declarations. The custom dimensions of a track are written when the track is
   created: the ones declared on its descriptor plus the ones inherited from its
   `parent_uuid` ancestors, deduplicating repeated identical values and
   reporting a producer error for a conflicting inherited value. A parent track
   is always created before its children, so this needs no post-import pass and
   no backfill. There are no separate custom process-, thread-, or GPU-dimension
   storage paths.

3. **trace_processor query surface.** Expose effective dimensions through one
   typed long-form relation:

   ```text
   track_dimension(
     track_id,
     name,
     int_value,
     string_value,
     display_name,
     is_well_known,
     display_value
   )
   ```

   Canonical values and presentation text are separate. The relation is
   materialized after EOF and indexed by `track_id`, so repeated slice/counter
   joins do not re-evaluate a compound view. Raw IIDs never escape into SQL.
   Provenance and generic template/numbering metadata are possible follow-ups,
   not prerequisites for the initial UI source-ID contract.

4. **Stable numbering (future).** Generalize the existing machine label index
   per `(source trace, machine, dimension name)`, ordering by canonical type and
   value rather than arrival. This is separate from the first custom-chip UI,
   which displays producer values or labels directly.

5. **UI.** Use the trace index, explicit source-ID contract and render-time chips
   described above. Details read stdlib presentation columns. Existing machine
   and GPU presentation remains unchanged until specialized ownership and
   migration behavior are reviewed.

### Custom dimensions and GPU work

GPU is a well-known dimension with the same trace_processor shape as machine,
CPU, process, thread, or a custom dimension. Its special behavior is limited to
identity merging and UI presentation.

Like any other track, a GPU track receives a custom dimension declared on a
process track — for example `rank` — only if it is a `parent_uuid` descendant of
that process track, e.g. a GPU track the producer emits under its own process
track. GPU tracks from other data sources, such as a hardware queue shared by
several processes, don't receive the custom dimensions of any process: their
work is attributed to a rank by joining the `upid` of the work to the process
track (see below). No GPU-specific custom-dimension code or per-domain dimension
table is needed. A cross-process GPU group does not receive a process dimension
when its children disagree: `rank` remains on the per-process child tracks while
the group carries only dimensions that have one unambiguous value, such as `gpu`
and `machine`.

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

-- GPU busy time per rank. The rank is declared on the process track, so GPU
-- work outside its parent_uuid subtree is attributed to a rank through the
-- upid of the work.
SELECT r.rank, sum(s.dur) AS gpu_busy
FROM gpu_slice AS s
JOIN (
  SELECT DISTINCT pt.upid, d.int_value AS rank
  FROM process_track AS pt
  JOIN track_dimension AS d ON d.track_id = pt.id
  WHERE d.name = 'rank'
) AS r USING (upid)
GROUP BY r.rank;
```

Process and thread remain typed top-level Perfetto concepts. Their tracks may
declare dimensions like any other track, and a process's custom dimensions are
the `track_dimension` rows of its process track; they do not require parallel
per-scope dimension tables. Reaching the tracks of a process's threads, or other
tracks only associated with the process, is an explicit join through `upid`
(`thread.upid` for threads) to the process track, as above. Machine continues to
reach every track because TP synthesizes it from each track's import context. A
track receives a custom dimension only from its own descriptor or an explicit
`parent_uuid` ancestor.

### Details and non-track surfaces

When a selected slice, counter, or other row has a backing `track_id`, the details
panel resolves and displays that track's effective dimensions. Process/thread
details use their process/thread tracks. Each entry shows the canonical name,
user-facing display value and optional display name; typed canonical IDs remain
available through SQL rather than being mistaken for PID/TID or CPU numbers.
This avoids copying custom columns into every event table while making dimensions consistently visible in
both SQL and details UI.

### Migration

- **Machine** → a well-known dimension available for every track through TP.
  Its existing name suffix stays in the initial UI. A later shared-label migration
  must define specialized ownership and be reviewed separately.
- **GPU** → a well-known dimension in the generic query relation. Existing
  hierarchy, names and presentation gates stay unchanged in V1.
- **Process/thread** → remain typed concepts; association is not dimension
  inheritance. Producers re-emit applicable custom dimensions in thread
  descriptors. Group UI annotations use explicit semantic source IDs.
- **Custom** → producers declare dimensions on the relevant tracks and their
  own `parent_uuid` subtrees. One core-owned index and render path labels all
  opted-in registrations. TrackEvent and TraceProcessorTrack enable ordinary
  sources by default; other plugins adopt the same metadata case by case.
  Adding a custom dimension requires no plugin-specific label query or pass.

Existing workspace subtitles and chips keep their current serialized shape.
Dimension chips are derived from trace data and are not workspace state.

## Alternatives considered

### Option 1 — Generic TP dimensions + shared presentation; GPU hierarchy untouched (recommended)

Adopt trace_processor's dimension vocabulary, add the producer surface for custom
dimensions, resolve all effective values into one typed track-keyed relation, and
provide a source-independent custom-chip resolver and details presentation.
Stage the migration of well-known labels separately. Do **not** add a generic
hierarchy mode; leave GPU's merging-based grouping specialized.

Pro:

- One query model for custom, machine, CPU, GPU, process, and thread
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
- Source mapping and agreement across multiplexed tracks/groups need care.
- Chips add horizontal density; changing specialized labels later needs review.

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
- **Shared well-known presentation.** Define specialized-handler ownership,
  defaults and migration before moving machine suffixes or changing GPU labels.
- **Generic numbering/templates.** Add deterministic numbering and templates
  without changing canonical identity.
- **User-configurable presentation.** Letting the user change how a dimension is
  surfaced (e.g. promote a label to its own subtree) and reorder dimensions at
  runtime.

## Remaining implementation questions

The review resolves the model-level questions: use `Dimension` / `dimensions`,
use derived custom chips in the initial UI, reserve the well-known names,
reject conflicting inherited values, intern string data and expose dimensions
in SQL and details UI. Generic numbering is separate future work. Implementation still needs
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
- **Chip presentation.** Validate density, overflow, tooltip and accessibility
  behavior against the existing track shell; no new subtitle row is introduced.
- **Future specialized ownership.** Agree on well-known label handling before
  expanding the custom-only resolver; do not make core depend on plugin policy.
