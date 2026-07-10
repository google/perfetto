# C SDK Reference

This page is a map of the Perfetto C SDK's public surface: the headers you
include, the functions and macros they expose, and — at the bottom — the
[ABI stability contract](#stability) that the rest of the documentation links to.

For task-oriented guides, start with the
[C SDK Getting Started](/docs/getting-started/c-sdk.md) tutorial and the
[Track Events](/docs/instrumentation/c-sdk-track-events.md) and
[Custom Data Sources](/docs/instrumentation/c-sdk-data-sources.md) how-tos.

WARNING: The C SDK is not yet stable. See [ABI stability](#stability) below
before depending on it.

## Header layout

The SDK is split into two layers. You compile against both, but only one is the
stability boundary.

- **`include/perfetto/public/*.h`** — a header-only convenience layer of
  `static inline` functions and macros. This code is compiled into *your*
  binary. It is where the ergonomics live (`PERFETTO_TE`, `PERFETTO_DS_TRACE`).
- **`include/perfetto/public/abi/*.h`** — the actual ABI boundary: opaque
  structs and `extern "C"` functions exported from `libperfetto_c`. The
  convenience layer calls into these. This is the surface stability applies to.

All headers compile as both C and C++.

## Convenience headers (`include/perfetto/public/`)

| Header | What it gives you |
| --- | --- |
| `producer.h` | Global init: `PerfettoProducerInit()`, `PerfettoProducerInitArgs`, backend selection, `PerfettoProducerActivateTrigger()`. |
| `track_event.h` | Track-event runtime: category/track registration (`PerfettoTeNamedTrackRegister`, `PerfettoTeCounterTrackRegister`), process/thread/global track UUIDs, flows, dynamic categories. |
| `te_category_macros.h` | Declare and register category lists: `PERFETTO_TE_CATEGORIES_DEFINE`, `PERFETTO_TE_REGISTER_CATEGORIES`. |
| `te_macros.h` | Emit events: the `PERFETTO_TE(...)` macro and its type/param macros (`PERFETTO_TE_SLICE_BEGIN`, `_INSTANT`, `_COUNTER`, `_ARG_*`, `_FLOW`, `_REGISTERED_TRACK`, …). |
| `data_source.h` | Custom data sources: `PERFETTO_DS_INIT`, `PerfettoDsRegister`, `PERFETTO_DS_TRACE`, packet begin/end, per-instance TLS and incremental state. |
| `tracing_session.h` | Create a consumer session (`PerfettoTracingSessionCreate`) for in-process recording. |
| `stream_writer.h`, `pb_msg.h`, `pb_macros.h`, `pb_packed.h`, `pb_utils.h` | Protozero serialization primitives used to fill in trace packets and configs. |
| `pb_decoder.h` | Iterator-based decoding for reading traces back. |
| `fnv1a.h`, `thread_utils.h`, `compiler.h` | Hashing, thread ids, portability macros (`PERFETTO_NULL`, `PERFETTO_STATIC_CAST`). |

## ABI headers (`include/perfetto/public/abi/`)

These declare the stable boundary. You rarely call them directly — the
convenience headers wrap them — but this is what `libperfetto_c` exports.

| Header | Boundary it defines |
| --- | --- |
| `export.h` | `PERFETTO_SDK_EXPORT` visibility control. |
| `backend_type.h` | `PERFETTO_BACKEND_IN_PROCESS` / `PERFETTO_BACKEND_SYSTEM`. |
| `producer_abi.h` | Producer/backend init entry points. |
| `tracing_session_abi.h` | Full session lifecycle (create/setup/start/stop/flush/read/destroy). |
| `data_source_abi.h` | Opaque data-source/tracer handles and lifecycle callbacks. |
| `track_event_abi.h` | `PerfettoTeInit()`, category and track registration. |
| `track_event_hl_abi.h` | High-Level track-event ABI — minimal call-site code size; what `PERFETTO_TE` uses. |
| `track_event_ll_abi.h` | Low-Level track-event ABI — iterate active instances and serialize protos by hand. |
| `stream_writer_abi.h`, `heap_buffer.h`, `pb_decoder_abi.h`, `atomic.h`, `thread_utils_abi.h` | Serialization, heap buffers, decoding, atomics, thread-id primitives. |

## Lifecycle at a glance

**Producer side** (the process being traced):

```c
struct PerfettoProducerInitArgs args = PERFETTO_PRODUCER_INIT_ARGS_INIT();
args.backends = PERFETTO_BACKEND_IN_PROCESS;  // or PERFETTO_BACKEND_SYSTEM
PerfettoProducerInit(args);
PerfettoTeInit();                              // if using track events
PERFETTO_TE_REGISTER_CATEGORIES(MY_CATEGORIES);
```

**Consumer side** (controlling an in-process trace):

```c
struct PerfettoTracingSessionImpl* session =
    PerfettoTracingSessionCreate(PERFETTO_BACKEND_IN_PROCESS);
PerfettoTracingSessionSetup(session, cfg_bytes, cfg_size);  // serialized TraceConfig
PerfettoTracingSessionStartBlocking(session);
// ... run the workload ...
PerfettoTracingSessionStopBlocking(session);
PerfettoTracingSessionReadTraceBlocking(session, read_cb, user_arg);
PerfettoTracingSessionDestroy(session);
```

See the [tutorial](/docs/getting-started/c-sdk.md) for the full, compilable version.

## High-Level vs Low-Level track events

Two ABIs back track events, trading code size against flexibility:

- **High-Level** (`track_event_hl_abi.h`) — used by `PERFETTO_TE`. The call site
  describes the event with a small argument list and the SDK does the
  serialization. Minimal generated code per event. Use this by default.
- **Low-Level** (`track_event_ll_abi.h`) — you iterate the active tracing
  instances yourself and serialize arbitrary protobuf per instance. More code
  per event, maximum control. Reach for it only when the High-Level API can't
  express what you need.

## {#stability} ABI stability & versioning

**The C SDK is not yet stable, and will not be for some time. Its API and ABI
are subject to change. Do not depend on it where you cannot tolerate breaking
changes.**

- There is **no date-based timeline**. Realistically stabilization is **at least
  a year out**, and that is a lower bound only.
- Stabilization is **gated on ongoing work to make the entirety of the Android
  OS build on this SDK**, and on shaking out the technical and architectural
  issues that surface along the way. It will be declared stable once that is
  proven in practice — not before.
- Until then, treat the surface as evolving: symbols, struct layouts, and
  behavior may change between releases.

This is the same message carried by
[`examples/shared_lib/README.md`](https://github.com/google/perfetto/blob/main/examples/shared_lib/README.md)
and the [API and ABI design doc](/docs/design-docs/api-and-abi.md).

### Why we still call it the foundation

The instability is about the *surface not being frozen yet*, not about the
design being unfinished. The mechanism that will make it stable already exists
and is worth understanding, because it is why the Rust and Java bindings can
already build on it:

- **Everything crossing the ABI is an opaque pointer or a plain-C function.**
  Internal representations (`PerfettoTracingSessionImpl`, `PerfettoDsImpl`, …)
  are never exposed by value, so their layout can change without breaking
  callers.
- **Structs that are *not* ABI-stable never cross the boundary.** For example
  `PerfettoProducerInitArgs` is documented as "not ABI-stable, fields can be
  added and rearranged." It is consumed only by the `static inline`
  `PerfettoProducerInit()` wrapper in *your* binary, which translates it into
  stable setter calls. New fields can be added without breaking the ABI.
- **Forward-compatible initializers** (`PERFETTO_PRODUCER_INIT_ARGS_INIT()`,
  `PERFETTO_DS_INIT()`, `PerfettoDsParamsDefault()`) let added fields default
  safely.
- **Export control** (`PERFETTO_SDK_EXPORT`) keeps only the intended symbols
  visible from `libperfetto_c`.

There is no numeric SDK version macro today; stability is structural (opaque
pointers + export control) rather than versioned. When the surface is frozen,
this section will document the compatibility guarantees.

## Source of truth

The headers themselves are the authoritative reference. Start from:

- `include/perfetto/public/` and `include/perfetto/public/abi/`
- Worked examples: `examples/shared_lib/example_shlib_track_event.c`,
  `examples/shared_lib/example_shlib_data_source.c`
- End-to-end in-process usage:
  `src/shared_lib/test/api_integrationtest.cc` and
  `src/shared_lib/test/utils.cc`
