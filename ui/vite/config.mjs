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

// Factory for Vite InlineConfig objects, one per JS bundle we ship. The caller
// (scripts/build.mjs) calls vite.build() once per config returned by this
// factory. Matches the old rollup.config.js topology:
//   frontend        -> dist/<ver>/frontend_bundle.js         (main SPA)
//   engine          -> dist/<ver>/engine_bundle.js           (wasm worker)
//   traceconv       -> dist/<ver>/traceconv_bundle.js        (converter worker)
//   chrome_extension -> <outDir>/ui/chrome_extension/        (MV3 background)
//   service_worker  -> dist/service_worker.js                (SW at dist root)
// We also support optional bundles:
//   bigtrace        -> dist/<ver>/bigtrace/bigtrace_bundle.js
//   open_perfetto_trace -> dist/open_perfetto_trace/open_perfetto_trace_bundle.js
//
// Output is IIFE and unhashed: the service worker's subresource integrity
// check relies on stable file names under dist/<ver>/, and the HTML bootstrap
// hardcodes <ver>/frontend_bundle.js.

import {join} from 'node:path';
import {UI_DIR, ROOT_DIR} from '../scripts/common.mjs';
import {
  pluginEmscriptenGlueToEsm,
  pluginLezerGrammarAlias,
  pluginProtoFixup,
  pluginTraceProcessor32Alias,
} from './plugins.mjs';

// Returns the list of bundle descriptors. Each descriptor is the input to
// `configForBundle()` below.
export function allBundles(layout, opts = {}) {
  const bundles = [
    {
      name: 'frontend',
      input: join(UI_DIR, 'src/frontend/index.ts'),
      outDir: layout.outDistDir,
      target: 'es2022',
      tsconfig: join(UI_DIR, 'tsconfig.json'),
    },
    {
      name: 'engine',
      input: join(UI_DIR, 'src/engine/index.ts'),
      outDir: layout.outDistDir,
      target: 'es2022',
      tsconfig: join(UI_DIR, 'tsconfig.json'),
      // self-scope: runs in a DedicatedWorkerGlobalScope. The existing code
      // narrows that intentionally via the selfWorker cast.
    },
    {
      name: 'traceconv',
      input: join(UI_DIR, 'src/traceconv/index.ts'),
      outDir: layout.outDistDir,
      target: 'es2022',
      tsconfig: join(UI_DIR, 'tsconfig.json'),
    },
    {
      name: 'chrome_extension',
      input: join(UI_DIR, 'src/chrome_extension/index.ts'),
      outDir: layout.outExtDir,
      target: 'es2022',
      tsconfig: join(UI_DIR, 'tsconfig.json'),
    },
    {
      name: 'service_worker',
      input: join(UI_DIR, 'src/service_worker/service_worker.ts'),
      outDir: layout.outDistRootDir,
      target: 'es2020',
      tsconfig: join(UI_DIR, 'src/service_worker/tsconfig.json'),
      outputFileName: 'service_worker.js', // NOT <name>_bundle.js
    },
  ];
  if (opts.bigtrace) {
    bundles.push({
      name: 'bigtrace',
      input: join(UI_DIR, 'src/bigtrace/index.ts'),
      outDir: layout.outBigtraceDistDir,
      target: 'es2021',
      tsconfig: join(UI_DIR, 'src/bigtrace/tsconfig.json'),
    });
  }
  if (opts.openPerfettoTrace) {
    bundles.push({
      name: 'open_perfetto_trace',
      input: join(UI_DIR, 'src/open_perfetto_trace/index.ts'),
      outDir: layout.outOpenPerfettoTraceDistDir,
      target: 'es2021',
      tsconfig: join(UI_DIR, 'src/open_perfetto_trace/tsconfig.json'),
    });
  }
  return bundles;
}

// Produces a Vite InlineConfig for one bundle. `layout` is the output layout
// returned by common.mjs::makeLayout(). `opts` carries global build options
// (minify, sourcemap, etc.).
export function configForBundle(bundle, layout, opts = {}) {
  const {
    minify = false, // false | 'esbuild' | 'terser'
    sourceMaps = true,
    mode = 'production',
  } = opts;
  const outFileName =
    bundle.outputFileName ?? `${bundle.name}_bundle.js`;

  return {
    // Skip auto-loading any ui/vite.config.* file. We only use programmatic
    // configs.
    configFile: false,
    // Serve/build at the UI dir root so relative imports resolve as they did
    // under the old tsc → rollup chain.
    root: UI_DIR,
    mode,
    // Vite's `base` affects URLs in HTML output. Our entries emit no HTML
    // (the index.html is handled separately), so base is irrelevant here.
    base: './',
    logLevel: opts.verbose ? 'info' : 'warn',
    clearScreen: false,
    // Replace references to process.env.NODE_ENV in 3rd-party code. Note
    // pluginProtoFixup does a broader textual replace as a safety net.
    define: {
      'process.env.NODE_ENV': JSON.stringify(mode),
    },
    resolve: {
      // Follow symlinks (Vite default). `ui/src/gen/` is now a real
      // directory that receives the staged codegen + wasm glue, so the
      // preserveSymlinks hack the old build needed is no longer required —
      // and enabling it actively confuses @rollup/plugin-commonjs on
      // pnpm-style node_modules (spurious externalization of e.g. ajv).
    },
    // Ensure Vite does not try to pre-bundle the Emscripten glue via
    // esbuild (which would break `document.currentScript?.src` access and
    // the CommonJS-vs-ESM detection at the end of the file).
    optimizeDeps: {
      exclude: [
        // Generated files live in the source tree via symlink and must be
        // consumed verbatim; let Vite / Rollup handle them in the final bundle.
      ],
    },
    // Tell esbuild what platform / features are available. The engine and SW
    // bundles technically don't have DOM but we don't need to split here; the
    // existing source code is careful about globals.
    esbuild: {
      target: bundle.target ?? 'es2022',
      // Vite default drops console+debugger only for production via terser.
      legalComments: 'none',
    },
    build: {
      outDir: bundle.outDir,
      emptyOutDir: false, // multiple Vite invocations share the same outDir
      target: bundle.target ?? 'es2022',
      sourcemap: sourceMaps,
      minify: minify === false ? false : minify,
      cssCodeSplit: false,
      // We intentionally produce single-IIFE bundles. Vite's default chunk
      // size warning fires at 500 KB; our frontend bundle is many megabytes
      // by design, so silence the heuristic.
      chunkSizeWarningLimit: Infinity,
      reportCompressedSize: false,
      // Emit a single self-contained file per entry. IIFE format means no
      // dynamic imports can be kept as separate chunks.
      rollupOptions: {
        input: {
          [bundle.name]: bundle.input,
        },
        output: {
          name: bundle.name,
          format: 'iife',
          inlineDynamicImports: true,
          entryFileNames: () => outFileName,
          // Even though IIFE inlines everything, Rollup will sometimes ask
          // about these; keep the names deterministic.
          chunkFileNames: '[name].js',
          assetFileNames: 'assets/[name][extname]',
          // Prevent Rollup from inserting `'use strict'` at every scope.
          strict: true,
        },
        // Emulate the old rollup.config.js treatment of circular deps:
        // warn for node_modules, fail for app code.
        onwarn(warning, defaultHandler) {
          if (warning.code === 'CIRCULAR_DEPENDENCY') {
            if (
              warning.message &&
              warning.message.includes('node_modules')
            ) {
              return;
            }
            throw new Error(
              `Circular dependency detected in ${warning.importer}:\n  ${(warning.cycle ?? []).join('\n  ')}`,
            );
          }
          defaultHandler(warning);
        },
      },
      commonjsOptions: {
        // Allow CommonJS files in the source tree (e.g. the Emscripten glue
        // under ui/src/gen/) to be transformed. Vite's defaults restrict CJS
        // to node_modules.
        include: [/node_modules/, /\/gen\//],
        // ajv's JIT validator compilation emits strings like
        //   `require("ajv/dist/runtime/equal").default`
        // which @rollup/plugin-commonjs tries to resolve as transitive deps
        // and then marks as external, leaving a broken `require$$N` in the
        // IIFE. Turn off dynamic-require analysis so those strings are left
        // untouched (they are only ever evaluated inside ajv's own codegen).
        ignoreDynamicRequires: true,
      },
    },
    plugins: [
      pluginLezerGrammarAlias(),
      pluginProtoFixup(),
      pluginTraceProcessor32Alias({genDir: layout.outGenDir}),
      pluginEmscriptenGlueToEsm(),
    ],
  };
}
