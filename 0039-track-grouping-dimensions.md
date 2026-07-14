# Unifying machine, GPU, and custom track grouping dimensions

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
  ` (machine N)` (or ` (<name>)`) suffix — a *label only*, no dedicated tree
  node. Implemented by a shared machine-label helper, a dense per-machine index
  (`label_index`) in the prelude `machine` view, and applied at ~10 track-naming
  call sites.

- **GPU.** In a multi-GPU trace, GPU tracks get an *extra hierarchy level* plus a
  `GPU N` / name label. Implemented in `dev.perfetto.Gpu`, with a separate,
  re-implemented per-process gate in `dev.perfetto.GpuByProcess`. The GPU code
  already reuses the machine-label helper and folds machine into its sort order —
  the two mechanisms already want to compose.

- **Custom, workload-defined dimensions (the gap).** There is real demand for
  another identifier that is generic and specific to the workload — declared by
  the producer rather than derived by the trace processor. The motivating example
  is the **rank** of a PyTorch distributed-training process, but "shard",
  "replica", "worker", "stage" are all the same shape. There is no way for a
  producer to declare such a dimension today, and adding one the current way would
  be a *third* hand-rolled copy of the same collapse-and-surface logic.

The data model is already partly generic — arbitrary track dimensions live in the
track's dimension arg set and are read via `extract_arg` — but there is no
producer surface for a generic labeled dimension (`TrackDescriptor` only has typed
`process`/`thread`/`counter`/`state`), and no shared grouping/labeling layer above
dimensions. We should define one concept that machine, GPU, and custom
dimensions are all instances of, instead of shipping a third system.

Throughout this RFC, **"custom dimension"** means a producer-declared,
workload-specific grouping dimension; **rank** is used only as a concrete example
of one.

## Decision

Pending

## Design

### The concept: a "grouping dimension"

A **grouping dimension** is a named identifier attached to tracks that the UI
turns into grouping and/or a label, applying one shared rule: *if the trace has a
single distinct value for it, it is invisible; if it has more than one, it is
surfaced.* Machine, GPU and any custom dimension are instances of this one
concept. A grouping dimension has:

- **name** — the dimension key in the track's dimension arg set (`machine`,
  `gpu`, or a custom name such as `rank`).
- **scope** — whose value the dimension carries, and where it acts in the tree: a
  process, a thread, or an individual track. Machine and (typically) custom
  dimensions are process-scoped; GPU is track-scoped. Machine is a partial
  exception: it acts at the process group, but its value is universal (a
  per-import-context column on *every* track), so it also labels global tracks
  (see Value resolution).
- **display** — a per-dimension label template (`machine %d`, `GPU %d`, `rank
  %d`; the registry's `display_name_template`) plus an optional per-value
  override string (a machine's or GPU's name, or a producer's `display_name`)
  that replaces the numbered default.
- **numbering** — how a stable, gap-free index is assigned to values for the
  default label (generalizes the existing per-machine index in the stdlib).
- **mode** — how it is surfaced when >1 value exists (below).

### Presentation modes

Each grouping dimension is surfaced in one of two modes:

- **LABEL** — appended as a suffix to the affected node's name, adding no tree
  node (today's machine behavior).
- **LEVEL** — inserts an extra grouping node in the track tree above the affected
  nodes (today's GPU behavior).

In v1 the mode is a **fixed per-dimension default**: machine → LABEL, GPU →
LEVEL, custom → LABEL. GPU is the only dimension that defaults to LEVEL, and it
keeps today's behavior. The key property is that mode (and order) are
*parameters of the shared grouping pass* below, resolved from the dimension's
registry entry — not constants hardcoded at each call site as they are today. That
is what lets user-configurable presentation be a clean follow-up rather than a
rewrite (see Future work); it is an explicit non-goal here.

### Composition and nesting

A single scope can carry several grouping dimensions at once — e.g. a GPU track on
`machine 1`, or a process on `machine 1` with a custom `rank 3`. Active grouping
dimensions form an **ordered list** and compose independently: LEVEL dimensions
nest in list order (outermost first), LABEL dimensions annotate the name of the
node they apply to, and the collapse-when-single rule is applied per dimension.

With the v1 defaults, LABEL and LEVEL already coexist. A GPU track on machine 1 in
a multi-GPU, multi-machine trace (GPU=LEVEL, machine=LABEL):

```text
GPU
└── GPU 0 (machine 1)          (LEVEL: gpu; LABEL: machine)
    └── <gpu tracks>
```

A process `trainer` on machine 1 with a custom `rank 3` (both LABEL) renders flat,
with both as suffixes:

```text
workspace
└── trainer (machine 1) (rank 3)   (process group; LABEL: machine + rank)
    └── <threads / tracks>
```

In a single-machine trace the machine dimension collapses. Choosing a *different*
mode/order (e.g. rank as a LEVEL subtree) is what the deferred reconfiguration
follow-up would expose; the composition mechanism itself ships now so machine,
GPU and custom dimensions can coexist.

### Interaction with `parent_uuid` and the process/thread hierarchy

This is the crux, because producers already build their own track hierarchies via
`TrackDescriptor.parent_uuid` (materialized as `track.parent_id`), and the UI
already builds a process → thread hierarchy. Grouping dimensions must layer on
top of both without disturbing them.

**How the tree is built today.** The `viz` grouping stdlib emits one row per UI
track group with its `upid`, `utid`, and `parent_id`. The UI nests each group
under:

- its `parent_id` group, if set — i.e. a producer `parent_uuid` subtree nests
  within itself; else
- the process group or thread group provided by
  `dev.perfetto.ProcessThreadGroups`; else
- the workspace root, for global tracks.

So every producer subtree has a **root** node whose parent is a process group, a
thread group, or the workspace root. Process/thread groups themselves attach at
the workspace root, with machine applied as a name suffix.

**The invariant.** *Grouping dimensions never reach inside a producer
`parent_uuid` subtree.* They act only at these existing attachment points, by
either annotating a node's name (LABEL) or re-parenting a subtree root under a
grouping node (LEVEL). A producer subtree always moves and stays intact as a
whole. Concretely, per scope:

- **Process-scoped (machine, custom).** Act on the process group node, which
  attaches at the workspace root. In LABEL mode the value is appended to the
  process group's name (today's machine behavior). In LEVEL mode a grouping node
  is inserted at the workspace root and process groups sharing a value are
  re-parented under it; everything below — threads and every producer subtree —
  rides along unchanged. The nesting is
  `[LEVEL dimensions, outer→inner] → process → thread → producer subtree`.
- **Track-scoped (GPU).** Act on the GPU track nodes at their existing location (a
  global GPU section, or per-process under `GpuByProcess`). In LEVEL mode the
  per-GPU node is inserted there — exactly today's behavior, now via the shared
  pass.

No v1 default puts a process-scoped dimension in LEVEL mode, but the mechanism
supports it (it is what the reconfiguration follow-up would drive).

**Value resolution.** A grouping dimension's value is resolved at its scope
(process / thread / subtree-root track); the whole subtree inherits it, so
producers tag only the scope root, not every child track. A value attached
*deeper* inside a producer subtree does **not** split that subtree — grouping
reads the root only (conflicting deeper values are an open question; the proposed
default is "root wins, never split"). A **global** track (no process/thread)
inherits no process/thread-scoped dimension; it participates only in track-scoped
dimensions it carries directly. Machine is the exception — its value is a per-track
column present on *every* track, including global ones — which is why machine
labels apply universally (see Querying).

This keeps two hard guarantees: producer-declared hierarchy is never broken up,
and the built-in process→thread nesting is preserved; grouping dimensions only add
outer levels or suffixes around them.

### Data model and pipeline

1. **Producer surface (new) — `TrackDescriptor`.** Add a repeated generic
   grouping dimension to `TrackDescriptor` (next free field is 21):

   ```proto
   message TrackDescriptor {
     // ... existing fields (uuid, parent_uuid, name, process, thread, counter,
     // state, ordering, ...) ...

     // Custom grouping dimensions declared by the producer, e.g.
     // {name: "rank", int_value: 3}. Scope is implied by this descriptor's kind
     // (see below). Repeated so a track can carry more than one.
     repeated GroupingDimension grouping_dimensions = 21;
   }

   message GroupingDimension {
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
     sub-descriptor) → *process-scoped*: declared once per process and
     inherited by all its threads/tracks. This is how a training workload sets
     `rank` once.
   - on a **thread** `TrackDescriptor` (`thread{}`) → *thread-scoped*.
   - on any **other** track's descriptor → *track-scoped* (that track/subtree).

   **trace_processor mapping.** Track-scoped dimensions map onto ordinary track
   dimensions via the existing blueprint path — **no storage schema change** — and
   are read via `extract_arg(dimension_arg_set_id, 'rank')`. Process/thread-scoped
   dimensions are recorded against the `upid`/`utid` (surfaced in the `viz`
   grouping as a per-scope property, the same way machine is a process property
   today, e.g. a small `_process_grouping_dimension(upid, name, value,
   display_name)` view). Derived dimensions (machine, GPU) keep being set by the
   importer as today.

2. **trace_processor.** A small stdlib registry enumerating known grouping
   dimensions and their metadata (name, scope, display_name_template, default
   mode), with machine and GPU seeded and custom dimensions discovered from the
   tracks that carry them. Generalize the existing per-machine index into a reusable
   "dense index per dimension value within scope". Expose a track's
   grouping-dimension values (and the owning scope's) from the `viz` grouping
   so the UI reads them in one query.

3. **UI.** Replace the per-identifier logic with one grouping/labeling pass over
   the attachment points above. Given a scope node, its grouping-dimension values,
   per-dimension distinct-value counts, and each dimension's (default) mode, it
   applies each active dimension: skip if a single distinct value; else apply LABEL
   (generalizing the machine-label + track-naming helpers) or insert a LEVEL node
   (generalizing the GPU group helper), using the registry display name, generic
   numbering, and a stable sort/dedup key. The machine and GPU one-offs collapse
   into two configurations of this one pass.

### Custom dimensions and GPU work

GPU tracks are importer-created and do not go through the `TrackDescriptor` path,
so they are not tagged with custom dimensions directly. In scope for this RFC, GPU
work picks up custom grouping *implicitly through its owning process*:
`dev.perfetto.GpuByProcess` already associates GPU tracks with a `upid`, so a
process's process-scoped dimensions (e.g. `rank`) group/label that process's GPU
tracks with no GPU-specific work — the GPU subtree rides along under the process
group like any other. The global, cross-process `dev.perfetto.Gpu` view groups by
GPU across processes, where a per-process value has no single meaning, so tagging
GPU tracks for that view is out of scope.

### Querying by a grouping dimension in trace_processor

Making these dimensions first-class also makes them a **query axis**, not just a
UI grouping — useful for ad-hoc SQL and for batch analysis across many traces
(`batch_trace_processor`, e.g. a per-rank metric over a whole job). Two additions
to the table surface:

- **A registry** so tools can discover a trace's dimensions:
  `grouping_dimensions(name, scope, display_name_template, default_mode)`.
- **Per-scope value tables** keyed by the scope's id, so a value joins to
  processes/threads/tracks. Process- and thread-scoped dimensions (which don't
  live on a track) get a long table, e.g.
  `_process_grouping_dimension(upid, name, value, display_name)` (and a thread
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
JOIN _process_grouping_dimension d USING (upid)
WHERE d.name = 'rank' AND d.value = 3;

-- GPU busy time per rank (gpu_slice carries upid via the GpuByProcess
-- association).
SELECT d.value AS rank, SUM(s.dur) AS gpu_busy
FROM gpu_slice s
JOIN _process_grouping_dimension d USING (upid)
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

- **Machine** → a process-scoped grouping dimension, default mode LABEL (its
  universal per-track value already reaches global tracks). The machine table
  and name stay; only labeling routes through the generic path.
- **GPU** → a track-scoped grouping dimension, default mode LEVEL, using its
  existing per-machine/global identity. Both GPU plugins call the shared pass and
  drop their duplicated count gates.
- **Custom** → producers emit the new dimension at the process scope (default mode
  LABEL); the process's GPU work is grouped implicitly via `GpuByProcess`. Adding
  a further custom dimension is then data/configuration only.

Existing traces render identically by default (labels/levels appear only with >1
value).

## Alternatives considered

### Option 1 — Full generic system; machine and GPU re-expressed on it (recommended)

Define the grouping-dimension concept end to end (producer surface → dimension →
registry + numbering → one shared UI pass over the attachment points) and migrate
machine and GPU onto it.

Pro:

- One implementation of collapse-and-surface; the custom case is configuration.
- Mode/order are parameters, so user-configurable presentation is a clean
  follow-up rather than a rewrite.
- Producers get a first-class way to express workload structure.

Con:

- Larger change; touches proto, stdlib, and several UI plugins.
- Re-expressing machine/GPU risks subtle diffs in existing names/ordering; needs
  diff-test coverage.

### Option 2 — Shared UI pass only; keep machine/GPU typed

Add the shared pass and the producer surface, but leave machine and GPU on their
current typed columns/dimensions, having them *call* the shared pass rather than
being *modeled* as instances.

Pro:

- Smaller, lower-risk; no churn to machine/GPU identity semantics.
- Still removes the duplicated gates and gives custom dimensions a home.

Con:

- Machine/GPU stay partly special-cased; two notions of a grouping dimension
  persist.

### Option 3 — Add each custom dimension the current way (do nothing generic)

Give a custom dimension (e.g. rank) its own dimension and its own hardcoded
collapse/label gate, like GPU got.

Pro:

- Minimal and self-contained; ships fastest.

Con:

- A third hand-rolled copy — exactly what this RFC exists to avoid, and it
  guarantees a fourth.

## Future work (non-goals of this RFC)

- **User-configurable presentation.** Letting the user change a dimension's mode
  (LABEL↔LEVEL) and reorder LEVEL dimensions at runtime, with the tree re-grouping
  live. This RFC deliberately ships fixed per-dimension defaults but keeps mode/
  order as parameters of the shared pass so this can be added on top without
  reworking the data model or pipeline.

## Open questions

- **Default modes.** Confirm per-dimension defaults (machine → LABEL, GPU →
  LEVEL, custom → LABEL), and whether a producer may *suggest* a default mode.
- **Default nesting order.** Canonical order when several LEVEL dimensions apply
  to the same scope (e.g. machine outermost, then custom dimensions).
- **Conflicting values within a producer subtree.** When a producer declares a
  grouping dimension with differing values on tracks inside one `parent_uuid`
  subtree: "root wins, never split" (proposed), ignore-below-root, or flag as an
  import error?
- **Producer proto shape.** The `GroupingDimension` value set (`int`/`string`
  only vs more types), interning for high-cardinality string values, and whether
  to reuse any existing annotation surface instead of a new message.
- **Query surface shape.** The stdlib shape: a long per-scope table
  (`_process_grouping_dimension(upid, name, value, display_name)`) + a
  resolve-one macro vs a wider/pivoted view; value typing (int vs string) in
  join predicates; and whether to add a universal track→`upid` resolver so one
  query can span thread, GPU and other slice domains rather than joining per
  domain.
- **Numbering stability & scope.** Stable indices across merged traces and across
  scopes (per-machine vs global), and how a producer-supplied `display_name`
  overrides numbering.
- **Migration risk / staging.** Re-expressing machine/GPU must not change existing
  names/order (diff-test sweep); go straight to Option 1 or stage via Option 2?
- **Non-track surfaces.** Whether the same dimensions should also annotate details
  tabs / SQL tables (machine id is shown raw there today).
