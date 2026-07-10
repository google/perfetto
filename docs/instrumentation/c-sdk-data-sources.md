# C SDK: Custom Data Sources

For most instrumentation,
[track events](/docs/instrumentation/c-sdk-track-events.md) are the right tool.
In rare cases they are not flexible enough — for example when the data doesn't
fit the notion of a track, or is high-volume enough that it needs a strongly
typed schema to keep each event small. For those cases, the C SDK lets you define
a **custom data source**.

This is an advanced feature. Unlike track events, a custom data source emits your
own protobuf messages, so you also need corresponding support in
[trace processor](/docs/analysis/trace-processor.md) to import your format.

WARNING: The C SDK is not yet stable — its API and ABI are subject to change.
See [ABI stability](/docs/reference/c-sdk-api.md#stability).

TIP: The complete runnable example is
[`examples/shared_lib/example_shlib_data_source.c`](https://github.com/google/perfetto/blob/main/examples/shared_lib/example_shlib_data_source.c).

## Registering a data source

A data source is represented by a single `PerfettoDs` object, initialized with
`PERFETTO_DS_INIT()` and registered under a unique name after the producer is
initialized:

```c
#include "perfetto/public/data_source.h"
#include "perfetto/public/producer.h"

static struct PerfettoDs custom = PERFETTO_DS_INIT();

int main(void) {
  struct PerfettoProducerInitArgs args = PERFETTO_PRODUCER_INIT_ARGS_INIT();
  args.backends = PERFETTO_BACKEND_SYSTEM;  // or PERFETTO_BACKEND_IN_PROCESS
  PerfettoProducerInit(args);

  PerfettoDsRegister(&custom, "com.example.custom_data_source",
                     PerfettoDsParamsDefault());
  // ...
}
```

The name (`com.example.custom_data_source`) is what you enable in the
[trace config](/docs/reference/trace-config-proto.autogen) to turn the data
source on.

## Emitting trace packets

Use the `PERFETTO_DS_TRACE` block macro to emit data. The body runs only when the
data source is enabled, and once per active tracing session:

```c
#include "perfetto/public/protos/trace/trace_packet.pzc.h"
#include "perfetto/public/protos/trace/test_event.pzc.h"

PERFETTO_DS_TRACE(custom, ctx) {
  struct PerfettoDsRootTracePacket root;
  PerfettoDsTracerPacketBegin(&ctx, &root);

  perfetto_protos_TracePacket_set_timestamp(&root.msg, PerfettoTeGetTimestamp());

  struct perfetto_protos_TestEvent for_testing;
  perfetto_protos_TracePacket_begin_for_testing(&root.msg, &for_testing);
  perfetto_protos_TestEvent_set_cstr_str(&for_testing, "Hello world!");
  perfetto_protos_TracePacket_end_for_testing(&root.msg, &for_testing);

  PerfettoDsTracerPacketEnd(&ctx, &root);
}
```

`PerfettoDsTracerPacketBegin` gives you a `TracePacket` to fill in using the
generated `.pzc.h` protozero accessors. Nested messages follow the same
`begin_*` / `set_*` / `end_*` pattern. Call `PerfettoDsTracerPacketEnd` to
finalize the packet.

`for_testing` above is a placeholder. In a real data source you would emit your
own message — an extension field on `TracePacket` — and teach trace processor to
parse it.

## Lifecycle callbacks and state

`PerfettoDsParamsDefault()` can be replaced with parameters that install
lifecycle callbacks (`on_setup_cb`, `on_start_cb`, `on_stop_cb`, `on_flush_cb`,
`on_destroy_cb`)
and per-instance state. This lets a data source, for example, enable hardware
counters on start and disable them on stop. See the `PerfettoDsParams` struct in
[`data_source.h`](https://github.com/google/perfetto/blob/main/include/perfetto/public/data_source.h)
and the [C SDK Reference](/docs/reference/c-sdk-api.md).

The SDK also supports per-instance thread-local storage
(`PerfettoDsGetCustomTls`) and incremental state (`PerfettoDsGetIncrementalState`)
for interning — useful for high-volume sources that repeat strings or ids.

## Recording

A custom data source is enabled like any other, by naming it in the trace config.
With the system backend, record with the standard
[system tracing](/docs/getting-started/system-tracing.md) tools. With the
in-process backend, control the session from your program as shown in the
[Getting Started](/docs/getting-started/c-sdk.md) tutorial, adding a data source
entry with your source's name to the `TraceConfig`.

## Next steps

- **[Track Events](/docs/instrumentation/c-sdk-track-events.md)**: the simpler,
  recommended path for most instrumentation.
- **[Custom Proto Extensions](/docs/instrumentation/extensions.md)**: how to
  define extension fields for the trace.
- **[C SDK Reference](/docs/reference/c-sdk-api.md)**: headers and functions.
