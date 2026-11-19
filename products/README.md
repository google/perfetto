# Products

Perfetto is a platform: recording, trace processing and a UI that can be put
together in many ways. A product is an opinionated tool built from those
pieces for one job, with its own entry points and its own site. Each
directory here is one product.

| Product | Site | What it is for |
|---|---|---|
| [memscope](memscope/) | memscope.perfetto.dev | Debugging the memory of one app |

## Layout

```
products/<id>/
  product.json   # id, title, origin; read by `ui/build --product <id>`
  README.md      # what the product is and how to use it
  cli/           # the command line entry point (C++, built with GN)
```

The product's UI shell lives in `ui/src/products/<id>/`, because the UI build
only compiles sources under `ui/src`. It is registered in
`ui/src/products/index.ts`. The plugins that do the product's work stay in
`ui/src/plugins/`, so they also run in ui.perfetto.dev.

## UI

A product's UI is an `Embedder` (`ui/src/core/embedder/embedder.ts`) that sets
the title, the home page and the plugin allowlist. The UI picks the product,
in order, from:

1. the `<meta name="perfetto-product">` tag written by `ui/build --product <id>`;
2. the `?product=<id>` query argument, for development;
3. the `<id>.perfetto.dev` hostname.

To work on a product's UI:

```sh
ui/run-dev-server
# then open http://localhost:10000/?product=memscope
```

To build it for deployment:

```sh
ui/build --product memscope
```

## CLI

A product CLI is a GN `executable` under `products/<id>/cli/`, added to
`all_targets` in the root `BUILD.gn`:

```sh
tools/ninja -C out/linux_clang_release memscope
```

CLIs hand traces to the UI by serving them once on `127.0.0.1:9001`, the only
port the UI's CSP allows, and opening `<origin>/#!/?url=...`.
