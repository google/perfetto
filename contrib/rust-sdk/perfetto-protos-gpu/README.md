# perfetto-sdk-protos-gpu

GPU event protobuf bindings for the [Perfetto](https://perfetto.dev) Rust SDK.

This crate provides auto-generated Rust types for GPU-related Perfetto
protobuf messages, including GPU render stage events, GPU counters, GPU
frequency events, GPU memory events, Vulkan events, and GPU track event
extensions.

It extends `TracePacket` from `perfetto-sdk` with GPU-specific fields so
trace producers can emit GPU events alongside standard track events.

## Usage

```rust,no_run
use perfetto_sdk_protos_gpu::protos::trace::trace_packet::prelude::*;
use perfetto_sdk_protos_gpu::protos::trace::gpu::gpu_counter_event::*;

fn write_gpu_counter(packet: &mut perfetto_sdk::protos::trace::trace_packet::TracePacket) {
    packet.set_gpu_counter_event(|event: &mut GpuCounterEvent| {
        event.set_counters(|counter: &mut GpuCounterEventGpuCounter| {
            counter.set_counter_id(1);
            counter.set_double_value(42.0);
        });
    });
}
```

## Related crates

| Crate | Description |
|-------|-------------|
| [`perfetto-sdk`](https://crates.io/crates/perfetto-sdk) | Main SDK with tracing session and track event APIs |
| [`perfetto-sdk-protos-trace-processor`](https://crates.io/crates/perfetto-sdk-protos-trace-processor) | Trace processor protobuf bindings |
