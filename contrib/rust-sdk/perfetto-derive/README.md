# perfetto-sdk-derive

Procedural macros for the [Perfetto](https://perfetto.dev) Rust SDK.

This crate provides the `#[tracefn]` attribute macro for automatically
instrumenting functions with Perfetto track events. When applied to a
function, it emits a trace event spanning the function's execution and
optionally captures input parameters as event arguments.

## Usage

```rust,ignore
use perfetto_sdk::track_event::*;

perfetto_sdk::track_event_categories! {
    pub mod my_categories {
        ("rendering", "Rendering events", []),
    }
}
use my_categories as perfetto_te_ns;

#[perfetto_sdk_derive::tracefn("rendering")]
fn draw_frame(width: u32, height: u32) {
    // A "draw_frame" trace event is emitted automatically,
    // capturing width and height as arguments.
}
```

## Related crates

| Crate | Description |
|-------|-------------|
| [`perfetto-sdk`](https://crates.io/crates/perfetto-sdk) | Main SDK with tracing session and track event APIs |
