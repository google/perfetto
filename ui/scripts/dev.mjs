#!/usr/bin/env node
// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Dev orchestrator. Vite-native dev mode:
//
//   1. Codegen + wasm stage (once).
//   2. Copy static assets + chrome extension assets. Production HTML is NOT
//      copied into the versioned dist dir in dev mode — the dev server
//      middleware reads ui/src/assets/index.html and serves a patched copy
//      at /.
//   3. sass --watch → dist/<ver>/perfetto.css
//   4. One-shot `vite build` for the bundles that almost never change:
//      traceconv, chrome_extension. They sit on disk as static files.
//   5. `vite build --watch` for the bundles we might edit: engine,
//      service_worker. Short rebuild times, don't hit the @rollup/plugin-
//      commonjs rebuild-cache bug that fires on the frontend.
//   6. Vite DEV SERVER for the frontend itself. Serves /src/** as native
//      ES modules, transformed on-demand by esbuild. A browser refresh
//      picks up source changes in milliseconds (no bundling step).
//   7. Watch dist/<ver>/ for worker/wasm/css changes and push a
//      `{type:'full-reload'}` message on Vite's HMR socket so the browser
//      reloads after a worker rebuild.
//
// The dev server uses Vite's publicDir feature to serve the pre-built worker
// bundles at their prod URLs (/v<ver>/*_bundle.js, /service_worker.js, *.wasm),
// so the frontend's `new Worker(assetSrc('engine_bundle.js'))` contract
// works identically in dev and prod.

import fs from 'node:fs';
import {join} from 'node:path';
import {
  UI_DIR,
  ROOT_DIR,
  ensureDir,
  ensureSymlinks,
  loadDevServerEnvFile,
  makeLayout,
  readVersion,
  resolveOutDir,
  spawnBg,
} from './common.mjs';
import {runAll as runCodegen} from './codegen.mjs';
import {stageWasm, buildWasm} from './build_wasm.mjs';
import {
  copyChromeExtensionAssets,
  copyStaticAssets,
} from './assets.mjs';
import {allBundles, configForBundle} from '../vite/config.mjs';

// ---------------------------------------------------------------------------
// CLI + env parsing. Precedence (lowest → highest):
//   1. Built-in defaults (`flags` below)
//   2. ~/.config/perfetto/ui-dev-server.env
//   3. PERFETTO_UI_* environment variables
//   4. CLI flags
// ---------------------------------------------------------------------------

function parseArgs(argv) {
  const flags = {
    out: null,
    host: null,
    port: null,
    crossOriginIsolation: false,
    title: '',
    onlyWasmMemory64: false,
    noBuild: false,
    noWasm: false,
    debug: false,
    bigtrace: false,
    openPerfettoTrace: false,
    noDepsCheck: false,
    noOverrideGnArgs: false,
  };

  loadDevServerEnvFile();
  const boolish = (x) => x === '1' || x === 'true';
  for (const [k, v] of Object.entries(process.env)) {
    if (!k.startsWith('PERFETTO_UI_')) continue;
    const n = k.slice('PERFETTO_UI_'.length).toLowerCase();
    switch (n) {
      case 'out': flags.out = v; break;
      case 'serve_host': flags.host = v; break;
      case 'serve_port': flags.port = parseInt(v, 10); break;
      case 'cross_origin_isolation': flags.crossOriginIsolation = boolish(v); break;
      case 'title': flags.title = v; break;
      case 'only_wasm_memory64': flags.onlyWasmMemory64 = boolish(v); break;
      case 'no_build': flags.noBuild = boolish(v); break;
      case 'no_wasm': flags.noWasm = boolish(v); break;
      case 'debug': flags.debug = boolish(v); break;
      case 'bigtrace': flags.bigtrace = boolish(v); break;
      case 'open_perfetto_trace': flags.openPerfettoTrace = boolish(v); break;
      case 'no_depscheck': flags.noDepsCheck = boolish(v); break;
      case 'no_override_gn_args': flags.noOverrideGnArgs = boolish(v); break;
    }
  }

  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    switch (a) {
      case '--out': flags.out = argv[++i]; break;
      case '--serve-host': flags.host = argv[++i]; break;
      case '--serve-port': flags.port = parseInt(argv[++i], 10); break;
      case '--cross-origin-isolation': flags.crossOriginIsolation = true; break;
      case '--title': flags.title = argv[++i]; break;
      case '--only-wasm-memory64': flags.onlyWasmMemory64 = true; break;
      case '--no-build': case '-n': flags.noBuild = true; break;
      case '--no-wasm': case '-W': flags.noWasm = true; break;
      case '--debug': case '-d': flags.debug = true; break;
      case '--bigtrace': flags.bigtrace = true; break;
      case '--open-perfetto-trace': flags.openPerfettoTrace = true; break;
      case '--no-depscheck': flags.noDepsCheck = true; break;
      case '--no-override-gn-args': flags.noOverrideGnArgs = true; break;
      // accepted no-ops.
      case '--serve': case '-s':
      case '--watch': case '-w':
      case '--verbose': case '-v':
      case '--no-source-maps':
      case '--no-treeshake':
      case '--typecheck':
        break;
      default:
        if (a.startsWith('--')) {
          console.warn(`Ignoring unknown flag ${a}`);
        }
    }
  }
  return flags;
}

// ---------------------------------------------------------------------------
// Worker builds: one-shot (traceconv/chrome_extension) or watch (engine/SW).
// ---------------------------------------------------------------------------

const WATCH_BUNDLES = new Set(['engine', 'service_worker']);
const ONESHOT_BUNDLES = new Set(['traceconv', 'chrome_extension']);

async function runOneShotBuild(bundle, layout, opts) {
  const vite = await import('vite');
  const cfg = configForBundle(bundle, layout, opts);
  const t0 = performance.now();
  await vite.build(cfg);
  console.log(
    `[vite:${bundle.name}] built in ${(performance.now() - t0).toFixed(0)}ms (one-shot)`,
  );
}

async function startWatchBuild(bundle, layout, opts) {
  const vite = await import('vite');
  const cfg = configForBundle(bundle, layout, opts);
  cfg.build = {
    ...cfg.build,
    watch: {
      exclude: ['out/**', '**/node_modules/**'],
      buildDelay: 250,
    },
  };
  const watcher = await vite.build(cfg);
  return new Promise((resolve) => {
    let first = true;
    watcher.on('event', (event) => {
      if (event.code === 'BUNDLE_END') {
        console.log(`[vite:${bundle.name}] bundled in ${event.duration}ms`);
        event.result?.close?.();
      } else if (event.code === 'END') {
        if (first) { first = false; resolve(); }
      } else if (event.code === 'ERROR') {
        console.error(
          `[vite:${bundle.name}] error:`,
          event.error?.message ?? event.error,
        );
      }
    });
  });
}

// ---------------------------------------------------------------------------
// Vite dev server: serves the FRONTEND as on-demand ES modules.
// ---------------------------------------------------------------------------

// Rewrites ui/src/assets/index.html for dev:
//   - Replaces the single line that synthesizes the frontend <script>
//     `script.src = version + '/frontend_bundle.js';`
//   - With the ESM-module equivalent pointing at /src/frontend/index.ts.
//   - Preserves all the error handling, preloads, channel logic above it.
// Also prepends a bare-script that sets window.__PERFETTO_ASSET_ROOT__ so
// `assetSrc()` in the frontend finds /v<ver>/* assets regardless of where
// the ES module itself loaded from.
function transformIndexHtmlForDev(html, version) {
  const assetRoot = `/${version}/`;

  // Swap the single line that would load the production bundle.
  // `script.src = version + '/frontend_bundle.js';`
  html = html.replace(
    /script\.src\s*=\s*version\s*\+\s*'\/frontend_bundle\.js';/,
    `script.type = 'module'; script.src = '/src/frontend/index.ts';`,
  );

  // Inject the asset-root global into <head>, before anything else runs, so
  // that assetSrc() has a defined value the moment the frontend module
  // starts evaluating.
  const assetRootInject = `\n  <script>window.__PERFETTO_ASSET_ROOT__ = ${JSON.stringify(assetRoot)};</script>\n`;
  html = html.replace(/<head>\n?/, (m) => `${m}${assetRootInject}`);
  return html;
}

// A no-op SSE endpoint at /live_reload. The frontend's core/live_reload.ts
// opens an EventSource to this path; without a handler it would log errors
// every few seconds. Returning a long-lived stream that never emits data
// keeps the browser EventSource happy without needing any client change.
function installLiveReloadStub(server) {
  server.middlewares.use('/live_reload', (_req, res) => {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      Connection: 'keep-alive',
    });
    // A comment line keeps the connection warm without delivering an event.
    res.write(':keep-alive\n\n');
  });
}

// Serves /test/** from the repo's test dir (Playwright fixtures, sample
// traces etc.). Mirrors what the old custom HTTP server did.
function installTestPassthrough(server) {
  server.middlewares.use('/test/', (req, res, next) => {
    const relPath = req.url?.split('?')[0]?.replace(/^\//, '') ?? '';
    const absPath = join(ROOT_DIR, 'test', relPath);
    // Block traversal.
    if (!absPath.startsWith(join(ROOT_DIR, 'test') + '/')) {
      res.writeHead(403); res.end(); return;
    }
    fs.stat(absPath, (err, stat) => {
      if (err || !stat.isFile()) return next();
      res.writeHead(200, {
        'Content-Type': 'application/octet-stream',
        'Content-Length': stat.size,
      });
      fs.createReadStream(absPath).pipe(res);
    });
  });
}

// Serves the prod index.html from ui/src/assets/index.html, patched for dev
// (see transformIndexHtmlForDev). Installed BEFORE publicDir so that "/"
// and "/index.html" are handled by us rather than fall through to static.
function installIndexMiddleware(server, {version, titleOverride}) {
  const htmlPath = join(UI_DIR, 'src/assets/index.html');
  server.middlewares.use((req, res, next) => {
    const url = req.url?.split('?')[0] ?? '';
    if (url !== '/' && url !== '/index.html') return next();
    fs.readFile(htmlPath, 'utf8', (err, raw) => {
      if (err) { res.writeHead(500); res.end(String(err)); return; }
      let html = transformIndexHtmlForDev(raw, version);
      // Stamp data-perfetto_version so the existing bootstrap's channel-map
      // logic sees a single-channel map (not the literal placeholder).
      html = html.replace(
        /data-perfetto_version='[^']*'/,
        `data-perfetto_version='${JSON.stringify({stable: version})}'`,
      );
      if (titleOverride) {
        html = html.replace(
          /<title>[^<]*<\/title>/,
          `<title>${titleOverride}</title>`,
        );
      }
      res.writeHead(200, {'Content-Type': 'text/html; charset=utf-8'});
      res.end(html);
    });
  });
}

async function startViteDevServer(layout, flags) {
  const vite = await import('vite');
  const {default: checker} = await import('vite-plugin-checker');
  const version = readVersion();

  // Reuse the bundle-shape for consistent resolve/define behavior across
  // dev and prod. The frontend bundle descriptor gives us the right target,
  // plugins, commonjsOptions, and 32-stub alias.
  const frontendBundle = allBundles(layout, {}).find(
    (b) => b.name === 'frontend',
  );
  const baseCfg = configForBundle(frontendBundle, layout, {
    minify: false,
    sourceMaps: false,
    mode: 'development',
    verbose: false,
  });

  // Project-wide tsc --noEmit running in a worker. Catches type errors in
  // files that aren't yet imported by the running frontend (otherwise lazy
  // loading would hide them). Errors land on the terminal AND in a clickable
  // browser overlay.
  const checkerPlugin = checker({
    typescript: {
      root: UI_DIR,
      tsconfigPath: 'tsconfig.json',
    },
    overlay: {
      initialIsOpen: false,
    },
  });

  const server = await vite.createServer({
    ...baseCfg,
    plugins: [...(baseCfg.plugins ?? []), checkerPlugin],
    appType: 'custom', // we handle / via our own middleware below
    publicDir: layout.outDistRootDir,
    // Pre-declare the frontend entry so Vite's initial dep-scan finds all
    // bare imports (mithril, codemirror, protobufjs, etc.) and pre-bundles
    // them in one pass at startup. Without this, Vite scans HTML <script
    // type=module> tags to discover entries — but our entry is injected by
    // our middleware, so the scanner sees nothing, optimizeDeps starts with
    // an empty set, then re-optimizes (with new chunk hashes) when the
    // browser actually loads the frontend. That re-optimization invalidates
    // chunks the browser is still requesting and produces transient
    // "The file does not exist at .../node_modules/.vite/deps/chunk-X.js" errors.
    optimizeDeps: {
      entries: ['src/frontend/index.ts'],
    },
    server: {
      host: flags.host ?? '127.0.0.1',
      port: flags.port ?? 10000,
      strictPort: flags.port !== null, // auto-increment only if no port was asked
      headers: flags.crossOriginIsolation
        ? {
            'Cross-Origin-Opener-Policy': 'same-origin',
            'Cross-Origin-Embedder-Policy': 'require-corp',
          }
        : undefined,
      watch: {
        // Vite's default includes out/; exclude it to avoid feedback loops.
        ignored: ['**/out/**', '**/node_modules/**'],
      },
      fs: {
        // The frontend imports generated files from ui/src/gen. Under Vite's
        // default fs.strict, these are allowed (they're inside root). If the
        // user has symlinks elsewhere, whitelist ROOT_DIR too.
        allow: [UI_DIR, ROOT_DIR],
      },
    },
    // No build: we're only running dev-serve.
    build: undefined,
  });

  installIndexMiddleware(server, {version, titleOverride: flags.title});
  installTestPassthrough(server);
  installLiveReloadStub(server);

  await server.listen();
  const {port, address} = server.httpServer.address();
  const host = address === '127.0.0.1' || address === '::1' ? 'localhost' : address;
  console.log(`Dev server listening on http://${host}:${port}/`);

  // Full-reload on worker/wasm/css changes.
  const wsReload = () => server.ws.send({type: 'full-reload'});
  let reloadTimer = 0;
  const scheduleReload = () => {
    if (reloadTimer) clearTimeout(reloadTimer);
    reloadTimer = setTimeout(wsReload, 250);
  };
  if (fs.existsSync(layout.outDistDir)) {
    fs.watch(
      layout.outDistDir,
      {recursive: true},
      (_eventType, relPath) => {
        if (!relPath) return;
        // Avoid reloading on source-map churn.
        if (relPath.endsWith('.map')) return;
        scheduleReload();
      },
    );
  }

  return server;
}

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------

async function main() {
  // vite-plugin-checker spawns `tsc` as a subprocess and expects it on PATH.
  // We invoke node from `ui/node` (not the system one), so PATH does not
  // automatically contain `node_modules/.bin`. Prepend it.
  const nodeBin = join(UI_DIR, 'node_modules/.bin');
  process.env.PATH = `${nodeBin}:${process.env.PATH ?? ''}`;

  const flags = parseArgs(process.argv.slice(2));
  const outDir = resolveOutDir(flags.out);
  ensureDir(outDir);
  const version = readVersion();
  const layout = makeLayout(outDir, version);

  if (!flags.noBuild) {
    ensureDir(layout.outUiDir, {clean: true});
  }
  ensureDir(layout.outUiDir);
  ensureDir(layout.outTscDir);
  ensureDir(layout.outDistRootDir);
  ensureDir(layout.outDistDir);
  ensureDir(layout.outExtDir);
  if (flags.bigtrace) ensureDir(layout.outBigtraceDistDir);
  if (flags.openPerfettoTrace) ensureDir(layout.outOpenPerfettoTraceDistDir);
  ensureSymlinks(layout);

  // 1. Codegen + wasm.
  runCodegen(layout);
  if (flags.noWasm) {
    stageWasm(layout, {
      onlyMemory64: flags.onlyWasmMemory64,
      debug: flags.debug,
    });
  } else {
    buildWasm(layout, {
      onlyMemory64: flags.onlyWasmMemory64,
      debug: flags.debug,
      noOverrideGnArgs: flags.noOverrideGnArgs,
    });
  }

  // 2. Static assets + chrome extension files. Note: we deliberately do NOT
  // copy the index.html into dist/<ver>/ in dev mode — the dev server serves
  // it directly from the source tree (patched) via middleware.
  copyStaticAssets({
    outDistDir: layout.outDistDir,
    outBigtraceDistDir: flags.bigtrace ? layout.outBigtraceDistDir : null,
  });
  copyChromeExtensionAssets({outExtDir: layout.outExtDir});

  // 3. SCSS in watch mode.
  const sassProc = spawnBg(
    join(UI_DIR, 'node_modules/.bin/sass'),
    [
      '--quiet',
      '--watch',
      join(UI_DIR, 'src/assets/perfetto.scss'),
      join(layout.outDistDir, 'perfetto.css'),
    ],
  );

  const opts = {
    minify: false,
    sourceMaps: false,
    mode: 'development',
    verbose: false,
  };

  // 4. One-shot bundles — traceconv and chrome_extension are rarely edited
  // and fast enough that a single build at startup is fine.
  const bundles = allBundles(layout, {
    bigtrace: flags.bigtrace,
    openPerfettoTrace: flags.openPerfettoTrace,
  });
  for (const b of bundles.filter((b) => ONESHOT_BUNDLES.has(b.name))) {
    await runOneShotBuild(b, layout, opts);
  }

  // 5. Watch bundles — engine (wasm worker, sometimes edited) and
  // service_worker (single tiny file). Both are small and don't hit the
  // commonjs rebuild-cache bug in practice.
  const watchBundles = bundles.filter((b) => WATCH_BUNDLES.has(b.name));
  await Promise.all(watchBundles.map((b) => startWatchBuild(b, layout, opts)));

  // 6. Vite dev server for the frontend.
  await startViteDevServer(layout, flags);

  // Clean shutdown.
  const shutdown = () => {
    try { sassProc.kill(); } catch { /* ignore */ }
    process.exit(0);
  };
  for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP']) {
    process.once(sig, shutdown);
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
