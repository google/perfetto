# RFC: Updatable Track Events

**Authors:** @etiennep-chromium

**Status:** Decided

## Problem

Currently, the Perfetto Track Event API provides `TRACE_EVENT_BEGIN` and `TRACE_EVENT_END` to define slices with duration. However, this model has limitations in a scenario common in Chrome:

### Late Tracing of Long-Running Operations (State Machines)

**Current Chrome Pattern (`_END` + `_BEGIN`):**
In [performance_manager](https://source.chromium.org/chromium/chromium/src/+/main:components/performance_manager/graph/properties.h), a `TracedWrapper` class observes tracing session starts to ensure properties are traced and emitted.

This is a single example, but this pattern is used by several components (about two dozen) throughout Chrome to emit long-running state.

When a new tracing session starts, `TrackEventSessionObserver::OnStart` is called, which triggers a re-emission of the state by ending and restarting the slice.

```cpp
// In properties.h (TracedWrapper)
void OnStart(...) override { Trace(); }

void Trace() {
  if (!TRACE_EVENT_CATEGORY_ENABLED("performance_manager.graph")) {
    return;
  }
  if (slice_is_open_) {
    TRACE_EVENT_END("performance_manager.graph", track_);
    slice_is_open_ = false;
  }
  perfetto::StaticString value_str = converter_func_(value_);
  if (value_str) {
    TRACE_EVENT_BEGIN("performance_manager.graph", value_str, track_);
    slice_is_open_ = true;
  }
}
```

**Drawbacks:**
*   For an *already running* tracing session, this unnecessarily breaks a logically single slice into multiple smaller slices every time a new tracing session starts or a value changes. This creates artificial track fragmentation and makes SQL analysis hard or incorrect when trying to overlap or intersect slice intervals.
*   The `_END` and `_BEGIN` events will sample two separate timestamps, even though there is conceptually only one event.

---

## Decision

Pending

## Design

PoC: https://github.com/google/perfetto/pull/5597

In practice, these state update don't need to support nested slices, so concerns about nesting are alleviated by explicitly choosing not to support it.

We introduce `TRACE_STATE` that takes a counter-like track argument, `StateTrack`, and thus cannot be mixed with `TRACE_EVENT_BEGIN`/`END`:

```cpp
class StateTrack : public Track {
 public:
  StateTrack(StaticName name,
             uint64_t id = 0,
             Track parent = MakeProcessTrack());
  ...
};

TRACE_STATE(cat, <value>, StateTrack(track), ...);
```

* TRACE_STATE supports all the same arguments (lambda, annotations,
  flows, etc.) that other TRACE_ macros support.
* A special "null" value (e.g. `nullptr`) needs to be supported to show the empty track.
* TRACE_STATE supports both string and proto enums as values.
* Emitting the same value with different arguments will augment the slice.
* Trace processor emits state "slice" to a separate table (name TBD, e.g. `state`) that contains values, timestamps and durations.
* Visually state slices are the same as regular slices in perfetto UI.

**Pros:**
*   Separates state machine updates from slice operations.
*   Alleviates concerns about slice nesting.

**Cons:**
*   Only solves one use case.

## Alternatives considered

### Option A: `TRACE_EVENT_STEP` (Original Proposal)

PoC: https://github.com/google/perfetto/pull/4167

Introduce a new track event type, `TYPE_SLICE_STEP`, and a corresponding macro, `TRACE_EVENT_STEP`.

`TRACE_EVENT_STEP` acts as an "upsert" operation on the track's slice stack:
*   If a matching incomplete slice is found on the stack, it updates that slice with any new arguments or flows provided.
*   If there is an incomplete slice whose name does not match, that slice is closed and a new slice is opened with the new name.
*   If no matching slice is found (e.g., because tracing started late), it starts a new slice, ensuring visibility.

**Pros:**
*   Solves both use cases with a single concept.
*   Handles late joining gracefully (becomes `BEGIN` if no slice found).
*   Keeps traces flat and clean without custom RAII helpers.

**Cons:**
*   Nesting can be confusing and will behave in a surprising way. Effectively, `TRACE_EVENT_STEP` only works for the leaf slice. This means we can't ensure multiple nested slices are present when starting a trace using `TRACE_EVENT_STEP`.

## Open questions

* Lalit voiced concerns that capturing now when emitting events at the start of a tracing session to describe state that started in the past is the wrong timestamp (this happens in both Option A and B, although this is true of the status quo as well).
