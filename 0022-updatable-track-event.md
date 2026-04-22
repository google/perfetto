# RFC: Updatable Track Events

**Authors:** @etiennep-chromium

**Status:** Draft

## Problem

Currently, the Perfetto Track Event API provides `TRACE_EVENT_BEGIN` and `TRACE_EVENT_END` to define slices with duration. However, this model has limitations in scenarios common in Chrome:

### Use Case 1: Late Tracing of Long-Running Operations (State Machines)

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

### Use Case 2: Dynamic Argument and Flow Augmentation (Deferred Data)

**Current Chrome Pattern (RAII Helper with Args at `END`):**
In cases like [render_frame_impl.cc](https://source.chromium.org/chromium/chromium/src/+/main:content/renderer/render_frame_impl.cc), arguments are passed to `TRACE_EVENT_END` to add details that became available only during the operation, using a custom RAII helper:

```cpp
// Emit the trace event using a helper as we:
// a) want to ensure that the trace event covers the entire function.
// b) we want to emit the new child routing id as an argument.
// c) child routing id becomes available only after a sync call.
struct CreateChildFrameTraceEvent {
  explicit CreateChildFrameTraceEvent(const LocalFrameToken& frame_token) {
    TRACE_EVENT_BEGIN("navigation,rail", "RenderFrameImpl::CreateChildFrame",
                       "frame_token", frame_token);
  }
  ~CreateChildFrameTraceEvent() {
    TRACE_EVENT_END("navigation,rail", "child_frame_token", child_frame_token);
  }

  LocalFrameToken child_frame_token;
};

void RenderFrameImpl::CreateChildFrame(...) {
 CreateChildFrameTraceEvent trace_event(frame_token_);
  // ...
  // child_frame_token becomes available only later in the method
  trace_event.child_frame_token = child_frame_token;
}
```

**Drawbacks:**
*   Arguments are only captured if the operation completes successfully and before tracing stops. There is no visibility into these arguments during the operation, and data is lost if the operation hangs or crashes.
*   Requires Chrome to implement a custom RAII helper.

## Decision

Pending

## Design

Pending decision on which solution to adopt. The solutions are discussed as alternatives below.

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

### Option B: Counter-like track with String (State Machines)

**Use Case Solved**: This only addresses Use Case 1 (State Machines) by allowing state changes to be traced on a non-stacking track.

In practice, Use Case 1 does not need to support nested slices, so concerns about nesting are alleviated by explicitly choosing not to support it.

We can introduce `TRACE_EVENT_STEP` that takes a counter-like track argument, `StateTrack`, and thus cannot be mixed with `TRACE_EVENT_BEGIN`/`END`:

```cpp
class StateTrack : public Track {
 public:
  StateTrack(StaticName name,
             uint64_t id = 0,
             Track parent = MakeProcessTrack());
  ...
};
```

* A special "idle" value (e.g. `nullptr`) needs to be supported to show the empty track.
* Flows and arguments should also be supported.
* Emitting the same value with different arguments should augment the slice.

**Pros:**
*   Separates state machine updates from slice operations.
*   Alleviates concerns about slice nesting.

**Cons:**
*   Only solves one use case.

### Option C: `TRACE_EVENT_UPDATE` (Deferred Data)

*   **Use Case Solved**: This addresses Use Case 2 (Deferred Data) by allowing arguments or flows to be attached to an active slice.

Introduce a macro `TRACE_EVENT_UPDATE` to update an existing slice without starting a new one if not found.

**Pros:**
*   Solves the argument augmentation use case cleanly.

**Cons:**
*   Only solves one use case.
*   It is unclear what the semantics should be when there is no incomplete slice.

## Open questions

*   Use Case 2 is discussed here because the original `TRACE_EVENT_STEP` (Option A) also solved it,
but it is unclear whether it is worth having a dedicated solution (Option C), if we implement Option B, as opposed to simply emitting an instant event. I would focus attention on solving Use Case 1 well.
* Lalit voiced concerns that capturing now when emitting events at the start of a tracing session to describe state that started in the past is the wrong timestamp (this happens in both Option A and B, although this is true of the status quo as well).
