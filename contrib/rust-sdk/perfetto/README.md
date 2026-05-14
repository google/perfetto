# perfetto-sdk

Safe and ergonomic Rust bindings for the
[Perfetto](https://perfetto.dev) tracing framework.

This crate provides the main API for recording trace data from Rust
applications. It wraps the Perfetto C API with safe Rust abstractions for
tracing sessions, data sources, and track events.

## Quick start

```rust,no_run
use perfetto_sdk::track_event::*;

perfetto_sdk::track_event_categories! {
    pub mod my_categories {
        ("rendering", "Rendering events", []),
        ("input", "Input events", []),
    }
}
use my_categories as perfetto_te_ns;

perfetto_sdk::track_event_instant!("rendering", "DrawFrame");
```

## Features

- **Track events** with categories, names, and typed arguments
- **Data sources** for custom trace data
- **Protozero encoding** in pure Rust for minimal overhead
- **Tracing sessions** for programmatic trace collection

## Crate features

| Feature | Default | Description |
|---------|---------|-------------|
| `vendored` | yes | Statically links the bundled Perfetto C library |
| `intrinsics` | no | Enables branch-prediction hints to reduce trace overhead |

## Related crates

| Crate | Description |
|-------|-------------|
| [`perfetto-sdk-sys`](https://crates.io/crates/perfetto-sdk-sys) | Low-level FFI bindings |
| [`perfetto-sdk-derive`](https://crates.io/crates/perfetto-sdk-derive) | Proc macros for function tracing |
| [`perfetto-sdk-protos-gpu`](https://crates.io/crates/perfetto-sdk-protos-gpu) | GPU event protobuf bindings |
