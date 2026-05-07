# tracing-perfetto-sdk

A [`tracing-subscriber`](https://docs.rs/tracing-subscriber) `Layer` that
emits [Perfetto](https://perfetto.dev) track events via the
[`perfetto-sdk`](https://crates.io/crates/perfetto-sdk) crate.

This bridges the Rust `tracing` ecosystem with Perfetto's native tracing
infrastructure. Spans become duration slices and events become instant
events, with full support for debug annotations, source locations, and
integration with the Perfetto tracing service.

## Quick start

```rust,no_run
use tracing_subscriber::prelude::*;

tracing_perfetto_sdk::init();
tracing_subscriber::registry()
    .with(tracing_perfetto_sdk::PerfettoLayer::new())
    .init();

let span = tracing::info_span!("my_function", arg = 42);
let _guard = span.enter();
tracing::info!("hello from Perfetto");
```

## Features

- **Zero-cost when disabled** — category enable check is a single atomic load
- **Debug annotations** — span and event fields are captured as Perfetto debug annotations
- **Source locations** — file and line number are attached to every event
- **Service integration** — works with both in-process tracing and the system tracing service

## Crate features

| Feature | Default | Description |
|---------|---------|-------------|
| `vendored` | yes | Statically links the bundled Perfetto C library |

## Related crates

| Crate | Description |
|-------|-------------|
| [`perfetto-sdk`](https://crates.io/crates/perfetto-sdk) | The underlying Perfetto SDK bindings |
| [`tracing`](https://crates.io/crates/tracing) | The Rust tracing facade |
| [`tracing-subscriber`](https://crates.io/crates/tracing-subscriber) | Composable tracing subscriber layers |
