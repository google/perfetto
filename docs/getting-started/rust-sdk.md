# Recording Traces with the Rust SDK

In this guide, you'll learn how to:

- Use the Perfetto Rust SDK to add trace points to a Rust application.
- Record a trace containing your custom events.
- Use the `#[tracefn]` attribute macro for automatic function tracing.
- Use the `tracing` crate with Perfetto as a backend.
- Extend `TracePacket` with GPU event protos.

NOTE: The Rust SDK is a community-maintained project. It may not have
the same level of support, stability, or feature coverage as the
official C++ SDK.

The Perfetto Rust SDK provides safe and ergonomic bindings for the
Perfetto tracing framework. It wraps the Perfetto C API with Rust
abstractions for tracing sessions, data sources, and track events.

## Crates

The SDK is split into several crates:

| Crate | Description |
|-------|-------------|
| `perfetto-sdk` | Core SDK with tracing sessions, data sources, and track events |
| `perfetto-sdk-sys` | Low-level FFI bindings to the Perfetto C API |
| `perfetto-sdk-derive` | `#[tracefn]` proc macro for automatic function instrumentation |
| `perfetto-sdk-protos-gpu` | GPU event protobuf bindings extending `TracePacket` |
| `perfetto-sdk-protos-trace-processor` | Trace processor protobuf bindings |
| `tracing-perfetto-sdk` | `tracing-subscriber` Layer for Perfetto |

## Setup

Add the SDK to your `Cargo.toml`:

```toml
[dependencies]
perfetto-sdk = "1"
```

By default, this compiles and statically links the bundled Perfetto C
library. No external dependencies are required.

## Track events

Initialize Perfetto and define your tracing categories:

```rust
use perfetto_sdk::producer::*;
use perfetto_sdk::track_event::*;
use perfetto_sdk::{scoped_track_event, track_event_begin, track_event_end, track_event_instant};

// Define tracing categories. Each category can be independently
// enabled or disabled in the trace configuration.
perfetto_sdk::track_event_categories! {
    pub mod my_categories {
        ("rendering", "Events from the graphics subsystem", []),
        ("network", "Network upload and download statistics", []),
    }
}
use my_categories as perfetto_te_ns;

fn main() {
    Producer::init(
        ProducerInitArgsBuilder::new()
            .backends(Backends::IN_PROCESS)
            .build(),
    );
    TrackEvent::init();
    my_categories::register().unwrap();

    // Scoped event — ends when the scope exits.
    scoped_track_event!("rendering", "DrawPlayer",
        |ctx: &mut EventContext| {
            ctx.add_debug_arg("player_number",
                TrackEventDebugArg::Uint64(1));
        },
        |_| {}
    );

    // Manual begin/end events.
    track_event_begin!("rendering", "DrawGame");
    track_event_end!("rendering");

    // Instant event.
    track_event_instant!("rendering", "VSync");
}
```

## Collecting traces

### In-process tracing

For self-contained trace collection without a tracing service, create
a `TracingSession` with the in-process backend. See
`contrib/rust-sdk/perfetto/examples/tracing_session.rs` for a
complete working example.

### System tracing

To connect to a running Perfetto tracing service (`traced`), use the
system backend instead:

```rust
use perfetto_sdk::producer::*;

Producer::init(
    ProducerInitArgsBuilder::new()
        .backends(Backends::SYSTEM)
        .build(),
);
```

Your application acts as a producer, and the system tracing service
controls when tracing starts and stops. Record a trace using the
[system tracing](/docs/getting-started/system-tracing.md) tools.

## Automatic function tracing with `#[tracefn]`

The `perfetto-sdk-derive` crate provides a proc macro that
automatically instruments a function with a track event. It captures
all input parameters as debug annotations.

```toml
[dependencies]
perfetto-sdk = "1"
perfetto-sdk-derive = "1"
```

Then annotate functions with `#[tracefn]`:

```rust
// Assuming categories are defined as in the example above.
use perfetto_sdk::producer::*;
use perfetto_sdk::track_event::*;
perfetto_sdk::track_event_categories! {
    pub mod my_categories {
        ("rendering", "Events from the graphics subsystem", []),
    }
}
use my_categories as perfetto_te_ns;
use perfetto_sdk_derive::tracefn;

#[tracefn("rendering")]
fn draw_player(player_number: u32, x: f64, y: f64) {
    // A "draw_player" track event is emitted automatically.
    // player_number, x, and y are captured as debug annotations.
}
```

The macro wraps the function body in a `scoped_track_event!` so the
event spans the full function execution. The category name is passed
as the macro argument.

## Using the `tracing` crate

If your application uses the Rust
[`tracing`](https://crates.io/crates/tracing) crate, you can use
`tracing-perfetto-sdk` to send events to Perfetto without changing
your existing instrumentation:

```toml
[dependencies]
tracing = "0.1"
tracing-subscriber = "0.3"
tracing-perfetto-sdk = "1"
```

Initialize and install the layer:

```rust
use tracing_subscriber::prelude::*;

tracing_perfetto_sdk::init();
tracing_subscriber::registry()
    .with(tracing_perfetto_sdk::PerfettoLayer::new())
    .init();

// Standard tracing macros emit Perfetto events.
let _span = tracing::info_span!("DrawGame").entered();
tracing::info!(player = 1, "drawing player");
```

Spans become duration slices and events become instant events, with
fields captured as debug annotations and source locations attached
automatically.

## GPU event protos

The `perfetto-sdk-protos-gpu` crate extends `TracePacket` with
GPU-specific fields for emitting GPU counter events, render stage
events, Vulkan events, and more.

```toml
[dependencies]
perfetto-sdk = "1"
perfetto-sdk-protos-gpu = "1"
```

Import the `TracePacketExt` trait to access the GPU fields:

```rust
use perfetto_sdk_protos_gpu::protos::trace::trace_packet::prelude::*;
use perfetto_sdk_protos_gpu::protos::trace::gpu::gpu_counter_event::*;

fn emit_gpu_counter(packet: &mut perfetto_sdk::protos::trace::trace_packet::TracePacket) {
    packet.set_gpu_counter_event(|event: &mut GpuCounterEvent| {
        event.set_counters(|counter: &mut GpuCounterEventGpuCounter| {
            counter.set_counter_id(1);
            counter.set_double_value(42.0);
        });
    });
}
```

This is typically used inside a custom data source's trace callback.
See `contrib/rust-sdk/perfetto-protos-gpu/examples/gpu_counters.rs`
for a complete example.

## Track event extensions

Track events can be extended with custom protobuf fields using the
extension mechanism. The `perfetto-sdk-protos-gpu` crate defines GPU
extensions such as `gpu_api` that tag track events with the GPU API
type.

Use `set_proto_fields` on `EventContext` to add extension fields:

```rust
// Assuming categories are defined as in the example above.
use perfetto_sdk::producer::*;
use perfetto_sdk::track_event::*;
use perfetto_sdk::track_event_instant;
perfetto_sdk::track_event_categories! {
    pub mod my_categories {
        ("rendering", "Events from the graphics subsystem", []),
    }
}
use my_categories as perfetto_te_ns;
use perfetto_sdk_protos_gpu::protos::trace::gpu::gpu_track_event::{
    GpuApi, TrackEventExtFieldNumber,
};

track_event_instant!("rendering", "cuLaunchKernel", |ctx: &mut EventContext| {
    ctx.set_proto_fields(&TrackEventProtoFields {
        fields: &[TrackEventProtoField::VarInt(
            TrackEventExtFieldNumber::GpuApi as u32,
            GpuApi::GpuApiCuda as u64,
        )],
    });
});
```

The extension field appears in the trace as `gpu_api: GPU_API_CUDA`
on the track event, which the trace processor decodes into the
`gpu_api` column of the `slice` table args.

## Next steps

- **[Track Events](/docs/instrumentation/track-events.md)**: Learn more
  about the different types of track events.
- **[Rust SDK examples](https://github.com/google/perfetto/tree/main/contrib/rust-sdk/perfetto/examples)**:
  Working examples of data sources, track events, and tracing sessions.
- **[GPU counter example](https://github.com/google/perfetto/tree/main/contrib/rust-sdk/perfetto-protos-gpu/examples)**:
  Example of a GPU counter data source.
