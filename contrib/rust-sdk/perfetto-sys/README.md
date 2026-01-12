# perfetto-sdk-sys

Low-level FFI bindings for Perfetto.

## Features

### `vendored` (default)

Compiles the Perfetto C library from the included amalgamated source. This is
the default and recommended option for most users.

### `bindgen`

Regenerates the Rust bindings from the C headers at build time using
[bindgen](https://github.com/rust-lang/rust-bindgen). This requires `libclang`
to be installed on the system.

By default, pre-generated bindings are used, which avoids the `libclang`
dependency and speeds up build times. Enable this feature when:

- You've modified the C headers and need updated bindings
- You're targeting a platform where the pre-generated bindings don't work
- You want to ensure bindings match your specific `libclang` version

```bash
cargo build --features bindgen
```
