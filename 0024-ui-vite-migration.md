# UI build migration to Vite

**Authors:** @primiano

**Status:** Proposal

**POC branch:** [dev/primiano/vite](https://github.com/google/perfetto/tree/dev/primiano/vite)


## Problem

The Perfetto UI build is driven by a hand-rolled `ui/build.js` (1177 LoC)
that orchestrates `rollup`, `tsc`, `sass`, `pbjs/pbts`, ninja, and a custom
HTTP server.

Pain points:

1. **Slow incremental dev rebuilds.** Editing `ui/src/frontend/sidebar.ts`
   triggers a `tsc` recompile (incremental, OK) followed by a rollup
   re-bundle of the whole frontend, ~3–5s per cycle once warm.
2. **A lot of bespoke code.** Live-reload SSE, port handling, asset
   copying, watch debouncing, build-lock, signal handling, all hand-rolled
   on top of node `http`, `fs.watch`, and `child_process`.


## Opportunities and risks

Opportunities:

- Move over to industry standard. Vite seems to have caught a lot of traction
 lately. It solves the problems we care about and has a large ecosystem.

- The speed up on dev cycle is incredible, it retains tsc debug output for
  type-related issues (altough does it asynchronously).

- More importantly works better with AI agents because there is no need to
  "wait for build", as the synchronization as transpilation happens on-demand
  when requesting a module from the HTTP server.

- It improves massively the dev workflow experience, as it doesn't require any
  bundling at all for dev.


Risks:

- There is a lot of complexity behind Vite, as it uses a miture of rollup
  bundles for production but for dev server, it pushes ES modules down to the
  browser and transpiles them on demand. Although this complexity is supposed to
  be dealt with by Vite.

- Realistically we are going to spend probably a few weeks shaking bugs here and
  there that will unavoidably come up.


## Proposal

Replace the entire `ui/build.js` + rollup orchestration with **Vite**:

* **Production builds** (`ui/build`): one `vite.build()` per IIFE bundle.
  Same output layout as before; same `manifest.json` + duplicated
  `index.html` post-build steps. Still bundles with rollup. 
  No behavioral change at the user/serving layer.
* **Dev mode** (`ui/run-dev-server`): use Vite's **dev server** for the
  frontend (native ES modules, esbuild per-file transforms, browser
  refresh in ~2s), and `vite.build()` for the worker / SW / chrome
  extension bundles (one-shot for the rarely-edited ones, `watch:{}` for
  the small ones we sometimes touch).

Hand-rolled bits we retire entirely:

* Custom HTTP server, `/live_reload` SSE, mtime 304s, gzip middleware
  (Vite dev server replaces them).
* `tsc --watch` as a JS emitter — Vite/esbuild transform `.ts` directly.
* Several rollup plugins (`re`, `sourcemaps2`, our own
  `embedMinimalSourceMap`).

We **keep**:

* `pbjs`/`pbts` invocations (with one option flip: `-w es6` so the dev
  server can serve the generated `protos.js` as a native module).
* `tools/gen_ui_imports`, `tools/write_version_header.py`,
  `tools/gen_stdlib_docs_json.py` — invoked by the new orchestrator.
* `tools/gn` + `tools/ninja` for wasm. Output staging unchanged.
* The `manifest.json` + duplicated `index.html` mechanism (required by
  the offline service worker).
* The single source-of-truth production `ui/src/assets/index.html` — a
  tiny middleware in dev rewrites *one line* of it on the wire to load
  the frontend as a module.

## Design

### Repository layout

```text
ui/
├── build                       # bash wrapper → scripts/build.mjs
├── run-dev-server              # bash wrapper → scripts/dev.mjs
├── scripts/
│   ├── common.mjs              # layout, exec helpers, env-file loading
│   ├── codegen.mjs             # protos, version, plugin index, stdlib docs
│   ├── build_wasm.mjs          # gn + ninja + stage outputs
│   ├── assets.mjs              # static asset + chrome-ext copy rules
│   ├── postbuild.mjs           # SW manifest.json + root index.html
│   ├── build.mjs               # production orchestrator
│   └── dev.mjs                 # dev orchestrator
└── vite/
    ├── config.mjs              # InlineConfig factory used by both modes
    └── plugins.mjs             # custom Vite plugins
```

`ui/src/gen/` is now a real directory (no longer a symlink) and receives
all generated files (codegen + Emscripten glue) directly. This sidesteps
a class of `pnpm` symlink interactions with `@rollup/plugin-commonjs`
that broke ajv resolution under `preserveSymlinks: true`.

### Production pipeline (`scripts/build.mjs`)

1. Parse args + env (`PERFETTO_UI_*`, `~/.config/perfetto/ui-dev-server.env`).
   CLI flags always win; precedence is documented in the parser.
2. Wipe `<outDir>/ui/` for a deterministic build; recreate the layout.
3. Codegen: pbjs+pbts → `ui/src/gen/protos.{js,d.ts}`; version header;
   plugin import index; stdlib docs JSON.
4. Wasm: optional `gn gen` + `ninja <mod>_wasm` for each module, then
   stage `.wasm` into `dist/<ver>/` and `.{js,d.ts}` glue into
   `ui/src/gen/`.
5. Copy fonts, pngs, catapult, chrome extension files; copy
   `ui/src/assets/index.html` into `dist/<ver>/index.html` verbatim.
6. Compile `ui/src/assets/perfetto.scss` → `dist/<ver>/perfetto.css`
   via the `sass` CLI.
7. **One `vite.build()` call per bundle** — sequentially, sharing the
   output dir via `emptyOutDir: false`. Output is forced to
   `<name>_bundle.js` (no hashing) so the existing service-worker
   subresource-integrity scheme keeps working.
8. Postbuild: walk `dist/<ver>/`, sha256 each non-`.map`/non-`index.html`
   file, write `manifest.json`. Duplicate `dist/<ver>/index.html` to
   `dist/index.html` with `data-perfetto_version` patched to a single-
   channel JSON map (`{"stable":"<ver>"}`) and optional `<title>`
   override.

The output tree is byte-equivalent to what the old rollup pipeline
produced (modulo bundle minifier output differences); the service worker
needs no changes.

### Dev pipeline (`scripts/dev.mjs`)

```text
ui/run-dev-server
  ├── codegen              (once)
  ├── stage wasm           (once; gn+ninja unless --no-wasm)
  ├── copy static assets   (once)
  ├── sass --watch         → dist/<ver>/perfetto.css
  ├── vite.build  one-shot → traceconv, chrome_extension
  ├── vite.build  watch:{} → engine, service_worker
  └── vite.createServer    → http://<host>:<port>/
        ├── publicDir = dist/   (serves /v<ver>/*, /service_worker.js,
        │                        /v<ver>/*.wasm, /v<ver>/perfetto.css)
        ├── middleware: read ui/src/assets/index.html and serve
        │   a transformed copy at / (see "HTML transform" below)
        ├── middleware: /live_reload no-op SSE stub
        ├── middleware: /test/** passthrough (Playwright fixtures)
        └── plugin: vite-plugin-checker (project-wide tsc --noEmit
            in a worker; errors land in terminal + browser overlay)
```

When a worker bundle changes on disk a Vite `ws.send({type:'full-reload'})`
is pushed so the browser refreshes automatically.

Edit-to-refresh budget on a warm cache (Linux, Node 20):

| Action                                | Time     |
|---------------------------------------|----------|
| Cold start (one-shot bundles + dev)   | ~5–6 s   |
| Edit frontend `.ts`, hit reload       | **~2 s** |
| Reload, no edits                      | ~1.4 s   |
| Edit `engine/index.ts`, watcher signals | ~0.5 s |
| Reload after engine rebuild            | ~2 s    |

### HTML transform

The production bootstrap in `ui/src/assets/index.html` ends with
roughly:

```js
const script = document.createElement('script');
script.async = true;
script.src = version + '/frontend_bundle.js';
script.onerror = () => errHandler(...);
document.head.append(script);
```

In dev, the dev server's middleware reads this same file and rewrites
*just* that line:

```js
script.type = 'module';
script.src = '/src/frontend/index.ts';
```

The error handler, the timeout, the CSS/font preloads, the
`data-perfetto_version` channel map — all run unchanged. The middleware
also injects a single inline script in `<head>` setting
`window.__PERFETTO_ASSET_ROOT__ = '/v<ver>/'` so that `assetSrc()`
(which historically derived the asset root from
`document.currentScript.src`) still works when the frontend is loaded
as a module (where `document.currentScript` is `null`). The
production HTML is **not** modified on disk.

The only source-side change required is in
`ui/src/base/http_utils.ts::getServingRoot()` — five lines that consult
the global before falling back to the existing `currentScript`-based
logic.

### Custom Vite plugins (`ui/vite/plugins.mjs`)

* **`pluginProtoFixup`** (build-only safety net): replaces
  `eval(...moduleName);` with `undefined;` in any `.js` we transform,
  and the `process.env.NODE_ENV` references that immer expects. This
  used to be `rollup-plugin-re`; same intent, more focused.
* **`pluginTraceProcessor32Alias`**: when the 32-bit `trace_processor`
  glue exists in `ui/src/gen/`, redirect `./trace_processor_32_stub`
  imports to it; otherwise leave the stub in place (which throws at
  runtime if the user's browser doesn't support memory64).
* **`pluginLezerGrammarAlias`**: `import {parser} from './foo.grammar';`
  is rewritten to `./foo.grammar.js`. Necessary because Vite resolves
  `./foo.grammar` to the literal text-format grammar file, then chokes
  parsing it as JS.
* **`pluginEmscriptenGlueToEsm`** (dev-server only): the
  `MODULARIZE=1` Emscripten glue ends with a CommonJS/AMD export
  trailer (`module.exports = X`). In `vite build` the rollup
  commonjs plugin handles it; in dev-serve there is no such plugin in
  the hot path, so we append `export default <name>_wasm;` to the
  served file. The trailing CJS/AMD branch becomes dead code.

### tsconfig change

`tsconfig.base.json` switched:

* `module: commonjs` → `module: esnext`
* `moduleResolution: node` → `moduleResolution: bundler`

`tsc` is no longer used for emit (Vite/esbuild do the transforms);
these settings only affect type-check resolution, which is now aligned
with what the bundlers actually do. Two minor source fixups in
`com.google.PerfettoMcp/{tracetools,uitools}.ts` add explicit `.js`
suffixes to the MCP SDK imports as the `bundler` resolver requires.

### Why `vite-plugin-checker`

In dev mode Vite only transforms files that the browser actually
imports. A type error in an unreached file (a disabled plugin, a new
file not yet wired up) would silently sit there. The plugin runs
`tsc --noEmit` over the whole project in a worker, surfaces errors
both in the terminal and as a click-through browser overlay. Verified
end-to-end by adding a synthetic `_test_checker_error.ts` containing
`const x: number = "string";` — the dev server logged the TS2322 even
though no file imported it.

## Alternatives considered

### A. Stay on rollup, retrofit watch-mode

Pros: smallest change.
Cons: doesn't address the "edit a frontend file → ~5s rebuild" pain.
Modern alternatives (Vite, rspack, esbuild) were designed exactly to
fix this; sticking with rollup leaves us with the slow path forever.

### B. `vite build --watch` for everything

This was the **original** attempt. It failed: editing the frontend
`.ts` triggered `[commonjs] Cannot read properties of undefined
(reading 'resolved')` from `@rollup/plugin-commonjs@28`'s rebuild-cache
path on the second build, leaving the watcher wedged. We tried two
escalations:

1. Spawn `vite build --watch` as a subprocess per bundle and respawn on
   that specific error code (worked but every frontend edit took ~21 s
   for a full cold rebuild — **unusable**).
2. Patch `meta.commonjs.resolved` access with a `?.` guard inline in
   `node_modules/.vite/.../dep-XXXX.js` and `@rollup/plugin-commonjs`,
   reapplied via a `postinstall` hook (worked, no more crashes, but
   the rebuild stayed at ~15 s because rollup was still re-bundling
   the whole frontend on every edit).

Both approaches were rejected for the chosen design (dev-server +
selective watch). The patches and the per-bundle subprocess scaffolding
have been removed.

### C. Webpack / rspack / turbopack

Pros: also have fast dev modes.
Cons: bigger config surfaces, less idiomatic for SPA + classic-script
workers.
Also the ecosystem seems to be shifting in favor of Vite.

### D. ESBuild directly (no bundler framework on top)

Pros: fastest possible builds.
Cons: 
- we'd be writing our own dev server + asset pipeline + CSS
  extractor + dep-pre-bundling all over again. That's exactly what we
  left behind.
- Rollup is still a better bundler (which vite uses in full buildS).

## Implementation notes / non-goals

* **Workers are still classic IIFE scripts.** `frontend` does
  `new Worker(assetSrc('engine_bundle.js'))` (no `{type: 'module'}`).
  The engine bundle is built as IIFE in both prod and dev. Switching
  to module workers would simplify some things but require source
  changes and has its own caveats (some Emscripten module-worker bugs
  in the past).
* **Source maps in dev are off by default.** Vite serves the original
  `.ts` source unmodified to the browser, so devtools can map line/col
  natively without us emitting `.map` files. This kept dev rebuild
  times tight; can be re-enabled per-bundle if desired.
* **Production source maps** are still emitted per Vite's defaults but
  the old `embedMinimalSourceMap` mechanism (a compact map registry on
  `self.__SOURCEMAPS[<bundleName>]`, consumed by
  `ui/src/base/source_map_utils.ts` for stack-trace decoration on
  error reports) was **not** ported. The runtime tolerates the
  registry being absent. Re-adding it as a Vite `generateBundle`
  plugin is left as a TODO that should be addressed.
* **Worker rebuild auto-reload** uses Vite's HMR websocket
  (`server.ws.send({type:'full-reload'})`) rather than the old SSE
  channel. The frontend's `core/live_reload.ts` was deleted along
  with its callers; the dev server still serves a no-op SSE stub at
  `/live_reload` for any out-of-tree consumer that pings it.

## Open questions / follow-ups

* **`isolatedModules: true`** in `tsconfig.base.json`. Would surface
  the handful of mixed value/type re-exports left in the codebase
  (`histogram.ts`, `track_helper.ts`). Currently both prod and dev
  build clean without it; it's a tightening choice, not a correctness
  one.
* **Per-bundle source-map strategy.** Re-enable for dev only? Match
  prod sizes? Bring back the embedded minimal-map registry?
* **`bigtrace` and `open_perfetto_trace`** smoke tests in a real
  browser. Plumbed through both pipelines but not visually verified
  post-migration.
