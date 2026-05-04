# perfetto-sdk-sys

Low-level FFI bindings for [Perfetto](https://perfetto.dev).

This crate provides raw Rust bindings to the Perfetto C API (`perfetto_c`).
It is used internally by `perfetto-sdk` and should not normally be used
directly.

## Crate features

| Feature | Default | Description |
|---------|---------|-------------|
| `vendored` | yes | Compiles and statically links the bundled Perfetto C library |
| `bindgen` | no | Regenerates Rust bindings from C headers at build time using bindgen (requires `libclang`) |

### Linking against an external library

To link against a system-installed Perfetto C library instead of the
vendored one:

```bash
export PERFETTO_SYS_LIB_DIR=/path/to/lib
cargo build -p perfetto-sdk-sys --no-default-features
```

## Related crates

| Crate | Description |
|-------|-------------|
| [`perfetto-sdk`](https://crates.io/crates/perfetto-sdk) | Safe wrapper around these bindings |
