# Extending TrackEvent with Custom Protos

Perfetto's trace format is extensible: you can attach your own strongly-typed
fields to `TrackEvent` without forking Perfetto or modifying its upstream proto
definitions. This is done with
[protobuf extensions](https://developers.google.com/protocol-buffers/docs/overview#extensions),
and it is a fully supported mechanism.

This is the recommended way to add custom structured data to your traces. It
works end-to-end: events are written with type-safe accessors from the C++
SDK or by emitting protobuf bytes directly when hand-generating a trace,
automatically parsed into the `args` table in Trace Processor, and displayed
in the Perfetto UI.

Use extensions when:

- You want more than unstructured debug annotations (strings, ints) — your event
  has structure that SQL queries will benefit from.
- You don't want to (or cannot) upstream your proto definitions to Perfetto.
- You need the same event schema across producers, Trace Processor, and the
  UI without coordinated rollouts.

This guide has two parts:

- [**Fundamentals**](#fundamentals) — defining the `.proto`, delivering
  descriptors to Trace Processor, and querying extension fields in SQL.
  Read this first; it applies to every producer.
- **Emitting events** — pick the path that fits your setup:
  - [With the C++ SDK](#emitting-events-with-the-c-sdk) — typed or inline
    field access from `TRACE_EVENT`.
  - [Without the SDK](#emitting-events-without-the-sdk) — any tool that
    writes Perfetto protobufs directly, e.g. when
    [converting arbitrary data to Perfetto](/docs/getting-started/converting.md).

## Fundamentals

### Defining an extension

Split your schema across two `.proto` files: a *data* file that defines the
nested message types your fields carry, and an *extension* file that hooks
those types onto [`TrackEvent`](/docs/instrumentation/track-events.md) at
specific field numbers. This split is what the Python walkthrough relies on
(the data file is compiled for runtime use; the extension file is only
compiled to a `FileDescriptorSet` for Trace Processor), and it keeps the C++
SDK layout symmetric.

**File 1 — `acme_data.proto`** is your regular data schema.

```protobuf
syntax = "proto2";
package com.acme;

message AcmeRequestMetadata {
  optional string endpoint = 1;
  optional uint32 priority = 2;
}
```

**File 2 — `acme_extension.proto`** is the extension *hook*. By convention,
place the `extend` block inside a wrapper message: this is required if you
plan to generate C++ SDK bindings — [Protozero](/docs/design-docs/protozero.md)
uses the wrapper message name as the generated class name — and recommended
regardless for portability.

```protobuf
syntax = "proto2";
import "protos/perfetto/trace/perfetto_trace.proto";
import "acme_data.proto";
package com.acme;

message AcmeExtension {
  extend perfetto.protos.TrackEvent {
    optional string request_id = 9900;
    repeated int32 retry_latencies_ms = 9901;
    optional AcmeRequestMetadata request_metadata = 9902;
  }
}
```

Place a copy of `perfetto_trace.proto`
([download from GitHub](https://github.com/google/perfetto/blob/main/protos/perfetto/trace/perfetto_trace.proto))
under `protos/perfetto/trace/` so the import resolves. The final layout:

```
project/
├── protos/perfetto/trace/perfetto_trace.proto   # from the Perfetto repo
├── acme_data.proto
└── acme_extension.proto
```

Field numbers 1000 and above are reserved for extensions. Pick a range that
won't collide with other extension producers you share traces with.

### Making extensions visible to Trace Processor and the UI

Trace Processor needs the proto descriptors for your extensions in order to
parse them. Once the descriptors are available, every extension field is
automatically decoded and inserted into the `args` table — no per-field
registration is required in Trace Processor itself.

There are three ways to deliver descriptors:

#### Option 1: Embed descriptors in the trace (`ExtensionDescriptor` packet)

This is the most portable option: the trace is self-describing, so Trace
Processor can parse it anywhere without extra configuration.

Compile your `.proto` to a `FileDescriptorSet` (e.g. `protoc --include_imports
--descriptor_set_out=acme.desc acme_extension.proto`) and prepend an
[`ExtensionDescriptor`](/docs/reference/trace-packet-proto.autogen#ExtensionDescriptor)
packet to the trace containing the bytes of that descriptor set.

The tracing service can do this automatically if you pass the descriptor set
into `TracingService::InitOpts::extension_descriptors` when starting the
service. Set `TraceConfig.disable_extension_descriptors = true` if you need to
opt out for a particular session.

For writers that don't use the C++ SDK, the
[synthetic track event walkthrough](/docs/reference/synthetic-track-event.md#proto-extensions)
shows this approach end-to-end in Python, including how to compile a
descriptor set and embed it in the trace.

#### Option 2: Android system-wide descriptors

On Android, `traced` reads descriptor sets from
`/etc/tracing_descriptors.gz` and `/vendor/etc/tracing_descriptors.gz` at
startup and emits them into every trace as `ExtensionDescriptor` packets. Ship
your extension's descriptor set to one of these paths to cover all traces
recorded on the device.

> NOTE: This was added to Perfetto in Feb 2026 via
> [RFC-0017](https://github.com/google/perfetto/discussions/4783), so it only
> works on Android releases that bundle a Perfetto build from that date or
> later — concretely, Android 16 QPR2 and later major releases. On earlier
> releases `traced` does not read these paths; use Option 1 or Option 3
> instead.

#### Option 3: Extension Servers (UI side)

If you run a shared [Extension Server](/docs/visualization/extension-servers.md)
for your team, add your descriptors to it. The Perfetto UI fetches descriptors
from the server at startup and uses them when opening any trace — no per-trace
embedding required. This is handy when the producers cannot be modified (e.g.
recordings from older versions).

### Querying extension fields in SQL

Every extension field that Trace Processor can decode is exposed in the
[`args`](/docs/analysis/sql-tables.autogen#args) table, keyed by the extension
field name. The easiest way to read a value is with the `EXTRACT_ARG`
built-in, which takes an `arg_set_id` and a key and returns the matching
value. Keys use dot notation for nested messages and `[N]` indexing for
repeated fields:

```sql
SELECT
  slice.name,
  EXTRACT_ARG(slice.arg_set_id, 'request_id') AS request_id,
  EXTRACT_ARG(slice.arg_set_id, 'request_metadata.endpoint') AS endpoint,
  EXTRACT_ARG(slice.arg_set_id, 'retry_latencies_ms[0]') AS first_retry_ms
FROM slice
WHERE EXTRACT_ARG(slice.arg_set_id, 'request_id') IS NOT NULL;
```

If you need to iterate over all elements of a repeated field, join against
the `args` table directly and filter by key prefix.

For interactive exploration, the Perfetto UI's details panel also displays
extension fields on the selected slice.

### Limitations

- Extensions are currently parsed by Trace Processor only for `TrackEvent`.
  Extending other messages works for writing but not for automatic
  args-table decoding.
- The C++ SDK's Protozero code generation requires extensions to live inside
  a wrapper message. Non-SDK producers can technically put the `extend`
  block at file scope, but the wrapper convention is recommended for
  portability.

## Emitting events with the C++ SDK

The [Tracing SDK](/docs/instrumentation/tracing-sdk.md) supports two styles of
extension emission.

### Typed field access

Pass your wrapper message as a template parameter to `ctx.event<...>()` to get
setters for the extended fields alongside all built-in `TrackEvent` fields:

```cpp
#include "acme_extension.pbzero.h"  // Generated from your .proto.

TRACE_EVENT("my_cat", "HandleRequest", [&](perfetto::EventContext ctx) {
  auto* event = ctx.event<perfetto::protos::pbzero::AcmeExtension>();
  event->set_request_id("req-42");
  event->add_retry_latencies_ms(12);
  event->add_retry_latencies_ms(34);
  event->set_request_metadata()->set_endpoint("/api/v1/search");
});
```

### Inline field access

For simple cases, pass field metadata and values directly as extra arguments to
`TRACE_EVENT`:

```cpp
TRACE_EVENT(
    "my_cat", "HandleRequest",
    perfetto::protos::pbzero::AcmeExtension::kRequestId, "req-42",
    perfetto::protos::pbzero::AcmeExtension::kRetryLatenciesMs,
        std::vector<int>{12, 34});
```

## Emitting events without the SDK

If you're hand-writing Perfetto protobufs — for example, from Python, Java,
or any other language while
[converting arbitrary data to Perfetto](/docs/getting-started/converting.md)
— extensions work the same way: set your extension field on the `TrackEvent`
message with your language's protobuf library, then deliver the descriptor
set as described in
[Making extensions visible to Trace Processor](#making-extensions-visible-to-trace-processor-and-the-ui).

For a complete worked walkthrough in Python — defining the `.proto` files,
compiling descriptors, emitting events with wire-format splicing, embedding
the descriptor set in the trace, and querying the result — see
[Attaching Custom Typed Fields with Proto Extensions](/docs/reference/synthetic-track-event.md#proto-extensions)
in the Advanced Guide to Programmatic Trace Generation.
