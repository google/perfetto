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
  README.md        # what the product is and how to use it
  build            # builds the CLI and the UI
  run-dev-server   # runs the UI dev server for this product
  cli/             # the command line entry point (C++, built with GN)
  ui/              # the UI shell; ui/index.ts is the entry point
  sql/<pkg>/       # PerfettoSQL package shared by the CLI and the UI
  configs/         # trace configs shared by the CLI and the UI
```

The plugins that do the product's work stay in `ui/src/plugins/`, so they
also run in ui.perfetto.dev. `products/<id>/ui` holds only what makes the
product a product: its embedder, home page and navigation.

## UI

A product is described entirely by its `Embedder`
(`ui/src/core/embedder/embedder.ts`): title, home page, plugin allowlist and
navigation. `products/<id>/ui/index.ts` exports it as `productEmbedder`.

The product is chosen at build time. `ui/build --product <id>` makes Vite
replace `ui/src/core/embedder/product_stub.ts` with
`products/<id>/ui/index.ts`; without `--product` the stub exports nothing
and the UI is the general purpose Perfetto UI.

Product UI code imports the Perfetto UI by relative path
(`../../../ui/src/...`). Bare imports such as `mithril` resolve through
`products/node_modules`, a symlink to `ui/node_modules` that `ui/build`
creates.

To work on a product's UI:

```sh
products/memscope/run-dev-server
```

Each product builds into its own `out/<id>_ui`, but takes the Wasm modules
from `out/ui`, which they do not depend on.

## CLI

A product CLI is a GN `executable` under `products/<id>/cli/`, added to
`all_targets` in the root `BUILD.gn`. `products/<id>/build` builds it along
with the UI.

CLIs hand traces to the UI by serving them once on `127.0.0.1:9001`, the only
port the UI's CSP allows, and opening `<origin>/#!/?url=...`.

## Shared SQL and configs

A product's analyses live in a PerfettoSQL package in `products/<id>/sql/<pkg>`,
so the CLI and the UI compute the same numbers with the same queries. Module
names follow the trace processor's convention: `sql/<pkg>/foo/bar.sql` is
`<pkg>.foo.bar`. The package is not part of the standard library.

`ui/build` bundles every `products/*/sql/<pkg>` and `products/*/configs/*` into
`ui/src/gen/products_sql.ts`, regenerated on change in watch mode. A plugin
registers its package with `app.addSqlPackages()` and then queries it with
`INCLUDE PERFETTO MODULE <pkg>.<module>`.

The CLI embeds the package with the `perfetto_amalgamated_sql_header` GN
template and registers it with `TraceProcessor::RegisterSqlPackage`. Configs
are embedded with `perfetto_cpp_blob_header`.

While developing, the package can be loaded into the shell directly:

```sh
trace_processor_shell --add-sql-package products/memscope/sql/memscope trace
```
