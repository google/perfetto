# C SDK: Track Events

Track events are the recommended way to instrument an application with the C SDK.
They cover the common tracing needs — durations, instants, counters, and the
relationships between them — behind the single `PERFETTO_TE` macro.

This page is a practical, example-driven tour. For the underlying concepts (what
a track is, how slices and flows behave), see the language-agnostic
[Track Events](/docs/instrumentation/track-events.md) page. For first-time setup,
start with the [Getting Started](/docs/getting-started/c-sdk.md) tutorial.

WARNING: The C SDK is not yet stable — its API and ABI are subject to change.
See [ABI stability](/docs/reference/c-sdk-api.md#stability).

TIP: Every snippet here is drawn from the runnable
[`examples/shared_lib/example_shlib_track_event.c`](https://github.com/google/perfetto/blob/main/examples/shared_lib/example_shlib_track_event.c).

## Categories

Every track event belongs to a category, which can be independently enabled or
disabled in the trace config. Declare your categories once with the X-macro list
and register them after `PerfettoTeInit()`:

```c
#define EXAMPLE_CATEGORIES(C)                        \
  C(rendering, "rendering", "Rendering events")      \
  C(physics, "physics", "Physics events", "tag1")

PERFETTO_TE_CATEGORIES_DEFINE(EXAMPLE_CATEGORIES)
// ...
PerfettoTeInit();
PERFETTO_TE_REGISTER_CATEGORIES(EXAMPLE_CATEGORIES);
```

Each entry is `C(identifier, "name", "description", optional tags...)`. The
identifier (e.g. `rendering`) is what you pass as the first argument to
`PERFETTO_TE`.

For categories whose name is only known at runtime, use a
[dynamic category](#dynamic-categories).

## Event types

The second argument to `PERFETTO_TE` selects the event type.

### Slices

Slices represent a duration. Begin and end are separate calls on the same track
(by default, the calling thread's track), and they nest:

```c
PERFETTO_TE(rendering, PERFETTO_TE_SLICE_BEGIN("DrawGame"));
PERFETTO_TE(rendering, PERFETTO_TE_SLICE_END());
```

`PERFETTO_TE_SLICE_END()` closes the most recently opened slice on that track.

### Instants

An instant is a single point in time:

```c
PERFETTO_TE(rendering, PERFETTO_TE_INSTANT("VSync"));
```

### Counters

A counter records a numeric value over time. It must target a counter track
(see [Custom tracks](#custom-tracks)):

```c
PERFETTO_TE(physics, PERFETTO_TE_COUNTER(),
            PERFETTO_TE_COUNTER_TRACK("Framerate", PerfettoTeProcessTrackUuid()),
            PERFETTO_TE_INT_COUNTER(120));
```

Use `PERFETTO_TE_DOUBLE_COUNTER(3.14)` for floating-point values.

## Debug annotations

Attach typed key/value annotations to any event. They appear under the `debug.`
namespace in the trace processor:

```c
PERFETTO_TE(physics, PERFETTO_TE_INSTANT("hit"),
            PERFETTO_TE_ARG_BOOL("critical", false),
            PERFETTO_TE_ARG_INT64("damage", 42),
            PERFETTO_TE_ARG_STRING("weapon", "laser"));
```

The available arg macros are `PERFETTO_TE_ARG_BOOL`, `_ARG_UINT64`, `_ARG_INT64`,
`_ARG_DOUBLE`, `_ARG_STRING`, and `_ARG_POINTER`.

## Timestamps

By default events are timestamped when the macro runs. To supply your own
timestamp (for example when replaying recorded work), use `PERFETTO_TE_TIMESTAMP`:

```c
PERFETTO_TE(rendering, PERFETTO_TE_INSTANT("frame"),
            PERFETTO_TE_TIMESTAMP(PerfettoTeGetTimestamp()));
```

## {#custom-tracks} Custom tracks

By default, slices and instants are recorded on the thread that emits them. To
place events on their own timeline, register a track and reference it.

Register a track once (typically at startup), parented to the process track:

```c
static struct PerfettoTeRegisteredTrack mytrack;
static struct PerfettoTeRegisteredTrack mycounter;

PerfettoTeNamedTrackRegister(&mytrack, "Renderer", /*id=*/0,
                             PerfettoTeProcessTrackUuid(),
                             /*is_name_static=*/true);
PerfettoTeCounterTrackRegister(&mycounter, "Framerate",
                               PerfettoTeProcessTrackUuid(),
                               /*is_name_static=*/true);
```

The final `is_name_static` argument declares whether `name` is a static string
literal whose pointer stays valid for the lifetime of the process. Pass `true`
for a string literal (as above); pass `false` for a name held in a buffer that
may be reused or freed.

Then target it with `PERFETTO_TE_REGISTERED_TRACK`:

```c
PERFETTO_TE(rendering, PERFETTO_TE_SLICE_BEGIN("Frame"),
            PERFETTO_TE_REGISTERED_TRACK(&mytrack));
PERFETTO_TE(rendering, PERFETTO_TE_SLICE_END(),
            PERFETTO_TE_REGISTERED_TRACK(&mytrack));

PERFETTO_TE(rendering, PERFETTO_TE_COUNTER(),
            PERFETTO_TE_REGISTERED_TRACK(&mycounter),
            PERFETTO_TE_INT_COUNTER(60));
```

For tracks whose name is computed at runtime, `PERFETTO_TE_NAMED_TRACK("name",
id, parent_uuid)` and `PERFETTO_TE_COUNTER_TRACK("name", parent_uuid)` avoid
pre-registration.

## Flows

Flows draw arrows between related events on different tracks — useful for
following a request across threads. Give both ends the same flow id:

```c
uint64_t id = next_request_id();
PERFETTO_TE(rendering, PERFETTO_TE_SLICE_BEGIN("enqueue"),
            PERFETTO_TE_FLOW(PerfettoTeProcessScopedFlow(id)));
// ... on another thread ...
PERFETTO_TE(rendering, PERFETTO_TE_SLICE_BEGIN("handle"),
            PERFETTO_TE_TERMINATING_FLOW(PerfettoTeProcessScopedFlow(id)));
```

`PERFETTO_TE_FLOW` continues a flow; `PERFETTO_TE_TERMINATING_FLOW` marks its
end.

## Correlation ids

Correlation ids group events that share an identity (such as a frame number) but
are not necessarily causally connected the way flows are:

```c
PERFETTO_TE(rendering, PERFETTO_TE_INSTANT("frame_begin"),
            PERFETTO_TE_CORRELATION_ID(frame_number));
```

## {#dynamic-categories} Dynamic categories

When the category name is only known at runtime, use the dynamic category
placeholder together with `PERFETTO_TE_DYNAMIC_CATEGORY_STRING`:

```c
PERFETTO_TE(PERFETTO_TE_DYNAMIC_CATEGORY, PERFETTO_TE_INSTANT("event"),
            PERFETTO_TE_DYNAMIC_CATEGORY_STRING("physics"));
```

## Reacting to enablement

To avoid expensive work when nobody is listening, register a callback that fires
when a category is enabled or disabled:

```c
static void OnEnabled(struct PerfettoTeCategoryImpl* c,
                      PerfettoDsInstanceIndex inst_id, bool enabled,
                      bool global_state_changed, void* user_arg) {
  // Start/stop collecting the data that feeds this category.
}

PerfettoTeCategorySetCallback(&physics, OnEnabled, PERFETTO_NULL);
```

## When track events aren't enough

Track events model timeline data (slices, counters, instants). If you need to
emit high-volume data with a strongly-typed schema of your own, use a
[custom data source](/docs/instrumentation/c-sdk-data-sources.md) instead.

## Next steps

- **[Custom Data Sources](/docs/instrumentation/c-sdk-data-sources.md)**: your own
  protobuf schema.
- **[Track Events model](/docs/instrumentation/track-events.md)**: the concepts
  behind slices, tracks, flows and counters.
- **[C SDK Reference](/docs/reference/c-sdk-api.md)**: the full macro and function
  list.
