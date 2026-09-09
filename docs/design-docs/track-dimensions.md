# Track dimensions (prototype)

_Status: prototype. Implements the model described in RFC 0039 "Unifying
machine, GPU, and custom track dimensions". The parts which are deliberately
not built yet are listed at the bottom._

## What a dimension is

A **dimension** is a named, typed key/value which is part of a track's
identity. Perfetto grew several independent mechanisms which all do the same
thing — take an identifier attached to a set of tracks and surface it in the UI
only when there is more than one distinct value — once for machines, once for
GPUs, and nothing at all for workload-specific identifiers such as the *rank*
of a distributed training process.

Dimensions are one vocabulary for all of these:

- **Well known dimensions** are recognized and synthesized by trace processor:
  `machine`, `gpu`, `cpu`, `process` and `thread`. The same canonical value in
  different data sources refers to the same real thing, which is what makes
  merging and specialized presentation possible.
- **Custom dimensions** are declared by the producer and are specific to the
  workload: `rank`, `shard`, `stage`, … Their identity is local to the
  producer, so they are labels and query keys, not merge keys.

## Producer surface

`TrackDescriptor.dimensions` is a repeated `Dimension`:

```proto
message Dimension {
  optional string name = 1;
  oneof value {
    int64 int_value = 2;
    string string_value = 3;
  }
  optional string display_name = 4;
}
```

The `display_name` only changes the *rendered* label — the canonical typed
value is what SQL and merging use.

With the SDK, dimensions are set like any other track metadata:

```cpp
auto desc = perfetto::ProcessTrack::Current().Serialize();
desc.mutable_process()->set_process_name("trainer");
auto* rank = desc.add_dimensions();
rank->set_name("rank");
rank->set_int_value(3);
perfetto::TrackEvent::SetTrackDescriptor(perfetto::ProcessTrack::Current(), desc);
```

A dimension has no scope field. It is declared on a track and applies to:

1. every track associated with the same process, if declared on a process
   track (including that process' threads),
2. every track associated with the same thread, if declared on a thread track,
3. the declaring track and its `parent_uuid` descendants otherwise.

Repeating the same name and value is deduplicated. Declaring a *different*
value for an inherited name is invalid: the inherited value is kept and
`track_dimension_conflicting_value` is recorded. Producers cannot declare a
custom dimension using a well known name
(`track_descriptor_reserved_dimension_name`).

## Trace processor

| Stage | Where |
|---|---|
| Parse `dimensions`, reject invalid or reserved names | `importers/proto/track_event_tokenizer.cc` |
| Record declarations against the process/thread/track they were declared on | `importers/proto/track_event_tracker.cc` → `__intrinsic_track_dimension_decl` |
| Resolve inheritance into effective per-track dimensions (whole-trace pass) | `importers/common/track_dimension_resolver.cc` → `__intrinsic_track_dimension` |
| Public query surface, incl. synthesized well known dimensions | `perfetto_sql/stdlib/prelude/after_eof/tracks.sql` |

Declarations are recorded even when the descriptor never gets a track of its
own (e.g. a process descriptor whose events all live on its threads); the
declaration is then anchored to the process or thread instead of a track.

Three public relations are projections of the same data:

- `track_dimension(track_id, name, int_value, string_value, display_name,
  is_well_known)`
- `process_dimension(upid, …)`
- `thread_dimension(utid, …)`

The process/thread projections exist because not every event inherits its
process from its track: a hardware GPU track can carry slices from several
processes, and `gpu_slice.upid` is what identifies the owner.

```sql
-- Slices on tracks whose effective rank is 3.
SELECT s.*
FROM slice AS s
JOIN track_dimension AS d USING (track_id)
WHERE d.name = 'rank' AND d.int_value = 3;

-- GPU busy time per rank, using the event's own upid.
SELECT d.int_value AS rank, sum(s.dur) AS gpu_busy
FROM gpu_slice AS s
JOIN process_dimension AS d USING (upid)
WHERE d.name = 'rank'
GROUP BY rank;
```

Well known dimensions are synthesized by the view from columns trace processor
already has (`machine_id`, `upid`, `utid`, and the `ucpu`/`ugpu` track
dimensions) rather than being stored a second time. Note this is deliberately
*not* the same thing as `track.dimension_arg_set_id`, which is the interning
key of a track and must not gain producer-declared values.

## UI

Presentation is factored into three pieces so that machine, GPU and custom
dimensions can share it:

- **Label helper** (`ui/src/public/dimensions.ts`): maps a canonical value to a
  label (`rank 3`, or the producer's display name), and lists the dimensions
  which have their own specialized presentation.
- **Collapse pass** (`ui/src/plugins/dev.perfetto.TrackDimensions`): a
  dimension is only labelled when it tells its peers apart, i.e. when the trace
  has more than one distinct value for it *or* some peers do not carry it at
  all. Dimensions collapse independently, and a label is not repeated on a node
  whose ancestor already shows it.
- **Details** (`ui/src/components/details/track_dimensions_section.ts`): shows
  the full effective dimensions of the selected event's track, with no collapse
  rule, so the timeline and the details panel read the same rows.

Labels are rendered as track **subtitles**. `TrackNode.subtitle` is composed of
independently owned items (`setSubtitleItem(id, text)`), so Chrome process
labels and dimension labels annotate the same node instead of overwriting each
other.

### Chrome process labels

Chrome's `ProcessDescriptor.process_labels` (renderer frame titles) fit this
model without extending it, which is a useful check that the abstraction is the
right shape:

- the labels are *ordered and repeated*, so they map to one dimension per
  label - `chrome.process_label`, `chrome.process_label2`, ... - rather than to
  one multi-valued dimension. Dimensions stay single-valued.
- a frame title is free text, so the producer sets `display_name` to the title
  and the label renders bare rather than as `chrome.process_label <title>`.
- typically only the renderer processes carry labels. This is why the collapse
  rule counts a missing dimension as a difference: with a single renderer there
  is only one distinct value, but the label still says something the other
  processes do not.

Nothing in this prototype migrates Chrome: it keeps emitting `process_labels`
and trace processor keeps exposing `chrome.process_label[i]` args.

```text
workspace
└── trainer 100                    (process group; name unchanged)
    ⤷ rank 3                       (subtitle label)
    ├── main 101                   (no label: same rank as its process)
    ├── pipeline A
    │   ⤷ stage forward
    └── pipeline B
        ⤷ stage backward
```

## Where the prototype differs from the RFC

Deliberate deviations, all of them at the edges of what the RFC pins down:

- **Inline strings instead of interned ids.** The RFC's Decision says custom
  string data is interned, and sketches `name_iid` / `string_value_iid` /
  `display_name_iid`. `TrackDescriptor` already carries `name` and
  `description` inline and a descriptor is emitted once per track, so the
  prototype uses inline strings. The RFC lists the interned-data layout as an
  open question, and iids can be added wire-compatibly.
- **Fewer columns in the query surface.** The RFC sketches `value_type`,
  `declaring_track_id` and `is_inherited`; the prototype omits all three.
  `value_type` is derivable (exactly one value column is non-null) and the RFC
  itself leaves provenance as an open question.
- **Well known dimensions are synthesized in the view**, not by the import
  path. The RFC describes typed descriptors and import context writing the same
  canonical rows as custom declarations; the prototype derives `machine`,
  `process`, `thread`, `cpu` and `gpu` from columns trace processor already has
  when the view is queried. The query surface is identical; nothing is stored
  twice.
- **Per-scope projections exist.** The RFC says process and thread "do not
  require parallel per-scope dimension tables". `process_dimension` and
  `thread_dimension` are views over the same declaration table rather than a
  parallel ingestion path, and they are needed because events such as
  `gpu_slice` carry their own `upid` instead of inheriting it from the track.
- **The collapse rule also fires on absence.** The RFC says a label is hidden
  when the applicable tracks have a single distinct value. The prototype also
  shows a dimension when some peers do not carry it at all, which is what makes
  Chrome-style labels (one labelled renderer among several processes) behave as
  they do today. It changes nothing for a dimension every peer carries.

## Not built yet

- **Machine and GPU still use their existing presentation.** Machine remains a
  ` (machine N)` name suffix and GPU keeps its extra hierarchy level; both are
  listed in `PRESENTED_ELSEWHERE` so the shared pass does not label them twice.
  The RFC moves the machine label to the subtitle immediately and has both GPU
  plugins consume the shared label helper; that is a mechanical change plus a
  diff-test sweep.
- **Stable numbering** per `(source trace, machine, dimension name)`, and the
  label templates (`machine %d`, `GPU %d`) which go with it: labels currently
  render `<name> <value>`, or the producer's display name. The registry
  therefore exposes only the well known bit.
- **Merging well known dimensions** across data sources (the path to folding
  GPU hierarchy into the shared model) remains future work, as in the RFC.
- **Details coverage** is limited to slice details panels. The RFC also wants
  dimensions on counter selections and in the process/thread details tabs.
- **Subtitle layout**: the full text is reachable through the tooltip and the
  accessible name, and CSS ellipsizes overflow, but height and wrapping for
  many labels on non-summary tracks still need design work.
