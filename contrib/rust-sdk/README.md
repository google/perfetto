# Perfetto Rust SDK

The **Perfetto Rust SDK** provides safe and idiomatic Rust bindings for the
[Perfetto tracing framework](https://perfetto.dev).
It allows Rust applications to produce and consume trace data, record track
events, and integrate with existing Perfetto infrastructure.

This SDK is designed to closely mirror the C API while providing the
ergonomics and safety guarantees of modern Rust.

---

## Overview

This workspace consists of three crates:

| Crate | Description |
|-------|--------------|
| [`perfetto-sdk-sys`](./perfetto-sys) | Low-level FFI bindings to the C API (`perfetto_c`). Can link against system or vendored builds. |
| [`perfetto-sdk`](./perfetto) | Safe and ergonomic wrapper around the raw FFI. Exposes the tracing session, data source, and track event APIs. |
| [`perfetto-sdk-derive`](./perfetto-derive) | Procedural macros for tracing the scope of function calls and automatically capturing all input parameters. |
| [`perfetto-sdk-protos-gpu`](./perfetto-protos-gpu) | Extra protobuf bindings for GPU events. |

---

## Features

- **Low-overhead tracing** — native atomics, branch prediction hints, and proto serialization using optimized rust code.
- **Safe API layer** — builder patterns, lifetime-checked handles, and error handling.
- **Track Event macros** — ergonomic category definition and event emission macros.
- **Protozero integration** — auto-generated Rust code from Perfetto `.proto` files via a protoc plugin.
- **Vendored or system builds** — link against a bundled `perfetto_c` library or use an external one.
- **FFI isolation** — `perfetto-sys` is the only crate exposing an API with `unsafe` code.
- **Cross-platform support** — Linux support is tested using CI.

---

## Building

### Prerequisites

- **Rust** ≥ 1.85 (edition 2024)
- Optionally: GN + Ninja for regenerating proto bindings

### Using Cargo

Build and run all tests:

```bash
# Links against a bundled `perfetto_c` library:
cargo test --manifest-path contrib/rust-sdk/Cargo.toml

# Links against an external `perfetto_c` library:
export PERFETTO_SYS_LIB_DIR=/path/to/lib
export PERFETTO_SYS_INCLUDE_DIR=/path/to/include
cargo test --no-default-features --manifest-path contrib/rust-sdk/Cargo.toml
```

#### Cargo Features

| Feature | Default | Description |
|----------|----------|-------------|
| `vendored` | True | Builds and statically links the bundled `perfetto_c` library. |
| `intrinsics` | False | Enables branch-prediction and fast-path intrinsics (`likely()`, `unlikely()`) to reduce trace overhead. |

---

## Developer Notes

Regenerating Proto Bindings

The Rust SDK uses a protoc plugin to generate Rust protozero code:

```bash
# Setup build with Rust SDK enabled:
gn gen out/rust

# Build protoc plugin and generate rust code from perfetto `.proto` files:
contrib/rust-sdk/tools/gen_rust_protos out/rust
```

This produces `*.pz.rs` files under `contrib/rust-sdk/perfetto/protos`.
