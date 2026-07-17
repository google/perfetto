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
3. Turn the collapse-and-label behavior into **shared label helpers** — so
   machine and custom dimensions use them directly, and GPU's existing hierarchy
   grouping calls the same helpers for its labels instead of re-implementing them.

Surfacing a dimension means attaching a **label**; it never inserts hierarchy.
Hierarchy for system-wide concepts (GPU today) comes from trace_processor merging,
not a UI grouping mode (see Two kinds of scoping and Alternatives).

Throughout this RFC, **"custom dimension"** means a producer-declared,
workload-specific dimension; **rank** is used only as a concrete example of one.

## Decision

Pending

## Design

### Terminology: dimensions

We align on trace_processor's existing concept. A **dimension** is a named
key/value attached to tracks (living in the track's dimension arg set, or, for
process/thread, keyed off `upid`/`utid`). There is one vocabulary, matching what
trace_processor already exposes. Process and thread fit this model conceptually,
but this RFC does not restructure them; modeling them as dimensions rides along
with the merging work (see Future work).

Dimensions fall into two categories:

- **Well-known dimensions** — recognized by trace_processor and shared across data
  sources: `machine`, `gpu`, `cpu`, and conceptually `process`/`thread`. Their
  defining property is that the *same value in different data sources refers to the
  same real thing*, so trace_processor can merge tracks that carry it (see below).
- **Custom dimensions** — producer-declared and workload-specific (`rank`,
  `shard`, `stage`, …). Standalone: their structure is the producer's, and they
  are surfaced as labels, not merged.

A dimension carries:

- **name** — the dimension key (`machine`, `gpu`, `rank`, …).
- **scope** — whose value it carries: a process, a thread, or an individual track.
  Machine and (typically) custom dimensions are process-scoped; GPU is
  track-scoped.
- **display** — a per-dimension label template (`machine %d`, `GPU %d`, `rank %d`)
  plus an optional per-value override string (a machine's or GPU's name, a
  producer's `display_name`) that replaces the numbered default.
- **numbering** — how a stable, gap-free index is assigned to values for the
  default label (generalizes the existing per-machine index in the stdlib).

### Two kinds of scoping

There is a useful distinction in how a producer's track event relates to the rest
of the trace. It shapes what "surfacing a dimension" should mean:

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
not in a UI mode layered over arbitrary producer trees. **This RFC builds only the
labeling layer (b-style, plus labels for well-known dimensions), and leaves
merging-based hierarchy as it is today.** Generalizing merging to more well-known
dimensions is the future direction (below), not this change.

### Presentation: labels only, as subtitles

The generic system surfaces a dimension in exactly one way: a **label**. A label
is a *secondary annotation on the affected node — not a mutation of its name*. The
target UI affordance is a track **subtitle** (secondary text under the track
name), modeled on Chrome's existing behavior; this decouples the
dimension from the name entirely and lets multiple dimensions coexist cleanly.

The one shared rule is unchanged: *if the trace has a single distinct value for a
dimension, it is invisible; if it has more than one, its label is shown.*

Several dimensions on one scope simply produce several labels (e.g. `machine 1`
and `rank 3` on a process), each collapsed independently. There is no nesting and
no re-parenting; labeling never touches the track tree.

A process `trainer` on machine 1 with a custom `rank 3` in a multi-machine,
multi-rank trace:

```text
workspace
└── trainer                        (process group; name unchanged)
    ⤷ machine 1 · rank 3           (labels shown as subtitle)
    └── <threads / tracks>
```

**No generic LEVEL mode.** GPU hierarchy is unaffected by this: it continues to
use its existing grouping in `dev.perfetto.Gpu` / `dev.perfetto.GpuByProcess`,
including its own hardcoded "more than one GPU" gate. What changes for GPU is only
that its `GPU N` / name **label** string is produced by the shared label helper
(display template + numbering) instead of a GPU-private copy; GPU applies that
string as its group-node name rather than as a subtitle:

```text
GPU
└── GPU 0                          (GPU's existing hierarchy grouping, unchanged)
    ⤷ machine 1                    (label via shared helper)
    └── <gpu tracks>
```

### Interaction with `parent_uuid` and the process/thread hierarchy

Because labeling never re-parents anything, its interaction with the existing tree
is simple: labels attach at the nodes that already exist and never reach inside a
producer `parent_uuid` subtree.

**Value resolution.** A dimension's value is resolved at its scope
(process / thread / subtree-root track); the whole subtree inherits it, so
producers tag only the scope root, not every child track. A value attached
*deeper* inside a producer subtree does not change grouping — grouping is the
producer's, via `parent_uuid`; we only read the scope root's value for the label
(conflicting deeper values are an open question; proposed default "root wins"). A
**global** track (no process/thread) inherits no process/thread-scoped dimension;
it participates only in track-scoped dimensions it carries directly. Machine is
the exception — its value is a per-track column present on *every* track, including
global ones — which is why machine labels apply universally (see Querying).

### Data model and pipeline

1. **Producer surface (new) — `TrackDescriptor`.** Add a repeated generic
   dimension to `TrackDescriptor` (next free field is 21):

   ```proto
   message TrackDescriptor {
     // ... existing fields (uuid, parent_uuid, name, process, thread, counter,
     // state, ordering, ...) ...

     // Custom dimensions declared by the producer, e.g. {name: "rank",
     // int_value: 3}. Scope is implied by this descriptor's kind (see below).
     // Repeated so a track can carry more than one.
     repeated Dimension dimensions = 21;
   }

   message Dimension {
     optional string name = 1;                 // dimension key, e.g. "rank"
     oneof value {
       int64 int_value = 2;
       string string_value = 3;
     }
     optional string display_name = 4;         // optional per-value label,
                                               // e.g. "worker-3"; else numbered
   }
   ```

   **Scope is implicit in which descriptor carries the field** — no separate scope
   enum needed:
   - on the **process** `TrackDescriptor` (the one with a `process{}`
     sub-descriptor) → *process-scoped*: declared once per process and inherited
     by all its threads/tracks. This is how a training workload sets `rank` once.
   - on a **thread** `TrackDescriptor` (`thread{}`) → *thread-scoped*.
   - on any **other** track's descriptor → *track-scoped* (that track/subtree).

   **trace_processor mapping.** Track-scoped dimensions map onto ordinary track
   dimensions via the existing blueprint path — **no storage schema change** — and
   are read via `extract_arg(dimension_arg_set_id, 'rank')`. Process/thread-scoped
   dimensions are recorded against the `upid`/`utid` (surfaced in the `viz`
   grouping as a per-scope property, the same way machine is a process property
   today, e.g. a small `_process_dimension(upid, name, value, display_name)`
   view). Derived dimensions (machine, GPU) keep being set by the importer as
   today.

2. **trace_processor.** A small stdlib registry enumerating known dimensions and
   their metadata (name, scope, `display_name_template`), with machine and GPU
   seeded and custom dimensions discovered from the tracks that carry them.
   Generalize the existing per-machine index into a reusable "dense index per
   dimension value within scope". Expose a scope's dimension values from the `viz`
   grouping so the UI reads them in one query.

3. **UI.** Factor the behavior into two reusable pieces: (i) a **label helper**
   that resolves a dimension value to its display string (registry template,
   per-value override, generic numbering, stable sort/dedup key), and (ii) a
   **generic label pass** that applies the collapse-when-single gate and renders
   the string as a subtitle. Machine's ~10 call sites and custom dimensions use
   the full pass. GPU's existing grouping keeps its hierarchy and its own hardcoded
   "more than one GPU" gate, but calls the label helper (i) for its `GPU N` string
   and applies it as its group-node name — dropping only its private
   label/numbering copy.

### Custom dimensions and GPU work

GPU tracks are importer-created and do not go through the `TrackDescriptor` path,
so they are not tagged with custom dimensions directly. In scope for this RFC, GPU
work picks up custom labels *implicitly through its owning process*:
`dev.perfetto.GpuByProcess` already associates GPU tracks with a `upid`, so a
process's process-scoped dimensions (e.g. `rank`) label that process's GPU tracks
with no GPU-specific work. The global, cross-process `dev.perfetto.Gpu` view
groups by GPU across processes, where a per-process value has no single meaning,
so tagging GPU tracks there is out of scope.

### Querying by a dimension in trace_processor

Making these dimensions first-class also makes them a **query axis**, not just UI
labeling — useful for ad-hoc SQL and for batch analysis across many traces
(`batch_trace_processor`, e.g. a per-rank metric over a whole job). Two additions
to the table surface:

- **A registry** so tools can discover a trace's dimensions:
  `dimensions(name, scope, display_name_template)`.
- **Per-scope value tables** keyed by the scope's id, so a value joins to
  processes/threads/tracks. Process- and thread-scoped dimensions (which don't
  live on a track) get a long table, e.g.
  `_process_dimension(upid, name, value, display_name)` (and a thread
  equivalent). Track-scoped dimensions need no new table — they are already on the
  track, read via `extract_arg(track.dimension_arg_set_id, '<name>')`.

Example extractions for a process-scoped custom dimension `rank`. Any per-domain
view that exposes `upid` joins the same way — `thread_slice` for thread slices,
`gpu_slice` for GPU slices (it carries its own `upid`), process-scoped counter
tracks, etc.:

```sql
-- Thread slices for rank 3.
SELECT s.*
FROM thread_slice s
JOIN _process_dimension d USING (upid)
WHERE d.name = 'rank' AND d.value = 3;

-- GPU busy time per rank (gpu_slice carries upid via the GpuByProcess
-- association).
SELECT d.value AS rank, SUM(s.dur) AS gpu_busy
FROM gpu_slice s
JOIN _process_dimension d USING (upid)
WHERE d.name = 'rank'
GROUP BY rank;
```

Machine works this way today too, with one difference worth calling out: machine's
value is a `machine_id` column on **every** `track` (stamped per import context),
so it also covers global tracks that have no process. A process-scoped custom
dimension instead attributes via `upid`, so it does not reach a global track — such
a track carries a custom dimension only if the producer tagged it track-scoped. A
universal helper that resolves a track's `upid` (and thus its process-scoped
dimensions) across all track types would let one query span thread, GPU and other
slice domains instead of joining per domain (see open questions).

### Migration

- **Machine** → a process-scoped dimension surfaced by the shared label helper
  (its universal per-track value already reaches global tracks). The machine table
  and name stay. Behavior change: the label moves from a name suffix to a
  subtitle, so machine no longer mutates the track name (see open questions).
- **GPU** → keeps its existing track-scoped hierarchy grouping unchanged, including
  its own hardcoded "more than one GPU" gate; both GPU plugins produce their
  `GPU N` label string via the shared label helper, dropping only their private
  label/numbering copy.
- **Custom** → producers emit the new dimension at the process scope; it renders as
  a subtitle label; the process's GPU work is labeled implicitly via
  `GpuByProcess`. Adding a further custom dimension is then data/configuration
  only.

Traces render identically by default (labels appear only with >1 value), except
for the machine suffix→subtitle move, which is a deliberate presentation change.

## Alternatives considered

### Option 1 — Labeling layer + shared helpers; GPU hierarchy untouched (recommended)

Adopt trace_processor's dimension vocabulary, add the producer surface for custom
dimensions, and extract collapse-and-label into shared helpers that machine,
custom, and GPU's label all use. Do **not** add a generic hierarchy mode; leave
GPU's existing merging-based grouping as is.

Pro:

- One implementation of collapse-and-label; the custom case is configuration.
- No generic LEVEL semantics, so no risky interaction with the track-event
  surface / producer `parent_uuid` trees.
- Producers get a first-class way to express workload structure; labels are
  decoupled from the name.
- Aligned with the trace_processor data model; process/thread retrofit cleanly.

Con:

- Does not yet unify GPU-style hierarchy under a single mechanism — GPU grouping
  stays special-cased (addressed by the merging future work, not here).
- Subtitle rendering is new UI surface that must be built.
- Moving machine off its name suffix needs a diff-test sweep.

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
  already merges process/thread across data sources to other well-known dimensions
  (gpu, machine, …), so that hierarchy for system-wide concepts falls out of
  trace_processor merging rather than any UI grouping mode. This is the path to
  eventually folding GPU's bespoke grouping into the shared model. Modeling
  process/thread themselves as dimensions (rather than the current parallel typed
  hierarchy) belongs with this work. Requires defining which dimensions are
  "well-known / mergeable" and how tracks from different data sources reconcile.
- **User-configurable presentation.** Letting the user change how a dimension is
  surfaced (e.g. promote a label to its own subtree) and reorder dimensions at
  runtime.

## Open questions

- **Subtitle affordance.** Track subtitles do not exist in the Perfetto UI today.
  Scope of the new affordance (multiple labels per node, styling, per-domain track
  support) and whether machine moves to it immediately or keeps its suffix until
  subtitles land (staging + diff-test).
- **Terminology / proto naming.** Confirm `Dimension` / `dimensions` on
  `TrackDescriptor` vs a more specific name, given `process`/`thread`/`counter`
  are also dimensions conceptually; and how the producer surface relates to any
  existing annotation surface.
- **Well-known vs custom boundary.** Which dimensions are treated as well-known /
  mergeable (machine, gpu, cpu, process, thread) and how that set is declared —
  relevant to the merging future work.
- **Conflicting values within a producer subtree.** When a producer sets a
  dimension with differing values on tracks inside one `parent_uuid` subtree:
  "root wins" (proposed), ignore-below-root, or flag as an import error?
- **Producer proto shape.** The `Dimension` value set (`int`/`string` only vs more
  types) and interning for high-cardinality string values.
- **Query surface shape.** The stdlib shape: a long per-scope table
  (`_process_dimension(upid, name, value, display_name)`) + a resolve-one macro vs
  a wider/pivoted view; value typing (int vs string) in join predicates; and
  whether to add a universal track→`upid` resolver so one query can span thread,
  GPU and other slice domains rather than joining per domain.
- **Numbering stability & scope.** Stable indices across merged traces and across
  scopes (per-machine vs global), and how a producer-supplied `display_name`
  overrides numbering.
- **Non-track surfaces.** Whether the same dimensions should also annotate details
  tabs / SQL tables (machine id is shown raw there today).
