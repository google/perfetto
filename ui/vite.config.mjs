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

// Minimal Vite config. Vite is used here purely as a TS transpiler + bundler
// replacement for tsc + rollup. It does NOT serve, watch assets, or process
// HTML / SCSS — build.mjs continues to own all of that.
//
// Inputs are the same entry points the old rollup.config.js used, but read
// directly from .ts source instead of from tsc's emit.

import {defineConfig} from 'vite';
import path from 'node:path';
import fs from 'node:fs';
import {fileURLToPath} from 'node:url';
import {lezer} from '@lezer/generator/rollup';
import checker from 'vite-plugin-checker';
import {pluginGenRelativeImports} from './vite/gen.mjs';
import {pluginPatchIndexHtml} from './vite/dev.mjs';
import {pluginPerfettoPluginBarrels} from './vite/plugins.mjs';
import {pluginPerfettoVersion} from './vite/version.mjs';
import {pluginPerfettoVirtualWasmModules} from './vite/wasm.mjs';
import {pluginEmbedMinimalSourceMap} from './vite/sourcemap.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT_DIR = path.dirname(__dirname);
const OUT_SYMLINK = path.join(ROOT_DIR, 'ui/out');

const NO_SOURCE_MAPS = process.env.NO_SOURCE_MAPS === 'true';
const NO_TREESHAKE = process.env.NO_TREESHAKE === 'true';
const MINIFY_JS = process.env.MINIFY_JS || '';
const ENABLE_BIGTRACE = process.env.ENABLE_BIGTRACE === 'true';
const ENABLE_OPEN_PERFETTO_TRACE =
  process.env.ENABLE_OPEN_PERFETTO_TRACE === 'true';
const IS_MEMORY64_ONLY = process.env.IS_MEMORY64_ONLY === 'true';
const DEV_VERSION = process.env.PERFETTO_DEV_VERSION || '';
const DEV_TITLE_OVERRIDE = process.env.PERFETTO_DEV_TITLE_OVERRIDE || '';

// IIFE bundles go to dist/v<version>/ so multiple releases can coexist in the
// GCS bucket. The version comes from build.mjs via PERFETTO_UI_VERSION (set
// from tools/write_version_header.py). Root-level files (service_worker.js,
// chrome extension) live outside the versioned dir.
const SRC = path.join(ROOT_DIR, 'ui/src');
const VERSION = process.env.PERFETTO_UI_VERSION || '';
const VERSIONED_DIST = VERSION ? `dist/${VERSION}` : 'dist';

// Dev-mode shim for the UMD wasm glue files in ui/src/gen/*.js. They end with
//   if (typeof exports === 'object' && typeof module === 'object') {
//     module.exports = X; module.exports.default = X;
//   }
// which Vite's native ESM serving doesn't recognise — the browser sees no
// `export default`. In build mode @rollup/plugin-commonjs handles this; in
// dev we transform the source: run the UMD body with a synthesised
// `module`/`exports`, then re-export `module.exports.default` as ESM default.
function pluginGenWasmGlueEsm() {
  return {
    name: 'perfetto:gen-wasm-glue-esm',
    enforce: 'pre',
    async load(id) {
      const file = id.split('?', 1)[0];
      if (
        !/\/gen\/(proto_utils|trace_processor(_memory64)?|traceconv)\.js$/.test(
          file,
        )
      )
        return null;
      const src = await fs.promises.readFile(file, 'utf8');
      // Wrap the UMD in a CJS-style shim and re-export as ESM.
      return `const module = {exports: {}};\nconst exports = module.exports;\n${src}\nexport default module.exports.default ?? module.exports;\n`;
    },
  };
}

// Per-bundle config: input file, output dir (relative to ui/out), and output
// filename. Most bundles follow the standard convention; service_worker and
// chrome_extension differ.
const BUNDLE_CONFIGS = {
  frontend: {dir: VERSIONED_DIST, entry: 'index.ts'},
  engine: {dir: VERSIONED_DIST, entry: 'index.ts'},
  traceconv: {dir: VERSIONED_DIST, entry: 'index.ts'},
  bigtrace: {dir: `${VERSIONED_DIST}/bigtrace`, entry: 'index.ts'},
  open_perfetto_trace: {dir: 'dist/open_perfetto_trace', entry: 'index.ts'},
  chrome_extension: {dir: 'chrome_extension', entry: 'index.ts'},
  service_worker: {
    dir: 'dist',
    entry: 'service_worker.ts',
    fileName: 'service_worker.js',
  },
};

// When invoked as `vite build`, BUNDLE selects one entry per invocation
// (build.mjs spawns one process per bundle). When invoked programmatically
// from build.mjs as a dev server (`createServer`), BUNDLE is unset and the
// frontend index.html drives the module graph.
const BUNDLE = process.env.BUNDLE;

export default defineConfig(({command}) => {
  const isBuild = command === 'build';
  if (isBuild && !BUNDLE) {
    throw new Error('vite.config.mjs requires BUNDLE=<name> env var for build');
  }
  const bundleCfg = isBuild ? BUNDLE_CONFIGS[BUNDLE] : null;
  if (isBuild && !bundleCfg) {
    throw new Error(`Unknown BUNDLE: ${BUNDLE}`);
  }
  const inputPath = isBuild ? path.join(SRC, BUNDLE, bundleCfg.entry) : null;
  const entryFileNames = isBuild
    ? bundleCfg.fileName || '[name]_bundle.js'
    : undefined;

  return {
    root: SRC,
    // Vite is used purely as a TS transpiler + bundler in build mode, and as
    // a module-serving dev server in serve mode. build.mjs owns HTML in both
    // modes (in dev it calls server.transformIndexHtml() and serves at /).
    appType: 'custom',
    // We have a real source directory at ui/src/public — disable Vite's
    // magic "publicDir" handling so it doesn't try to serve it at /.
    publicDir: false,
    plugins: [
      pluginPerfettoPluginBarrels({
        sources: [
          {exportName: 'plugins', dir: path.join(SRC, 'plugins'), prefix: ''},
          {
            exportName: 'corePlugins',
            dir: path.join(SRC, 'core_plugins'),
            prefix: 'core_',
          },
        ],
        virtualModule: path.join(SRC, 'virtual', 'plugins'),
      }),
      pluginPerfettoVersion({
        virtualModule: path.join(SRC, 'virtual', 'version'),
        scriptPath: path.join(ROOT_DIR, 'tools/write_version_header.py'),
      }),
      pluginPerfettoVirtualWasmModules({
        virtualDir: path.join(SRC, 'virtual'),
        genDir: path.join(SRC, 'gen'),
        isMemory64Only: IS_MEMORY64_ONLY,
      }),
      pluginPatchIndexHtml({
        devVersion: DEV_VERSION,
        devTitleOverride: DEV_TITLE_OVERRIDE,
      }),
      // build.mjs spawns one `vite build` per bundle in parallel. Running
      // vite-plugin-checker in every one of them would launch N racing tsc
      // processes against the same project; gate it to a single bundle.
      ...(BUNDLE === 'frontend'
        ? [checker({typescript: true, overlay: false})]
        : []),
      // Compiles *.grammar files (lezer parser definitions) on import. Replaces
      // the old "manually run lezer-generator and commit gen/*.js" workflow.
      lezer(),
      // pluginGenRelativeImports({genSymlink: path.join(SRC, 'gen')}),
      ...(isBuild ? [] : [pluginGenWasmGlueEsm()]),
      ...(NO_SOURCE_MAPS
        ? []
        : [
            pluginEmbedMinimalSourceMap({
              sourceReplacements: [
                ['../../../out/ui/', ''],
                ['../../node_modules/', 'node_modules/'],
              ],
            }),
          ]),
    ],
    resolve: {
      // NB: do NOT set preserveSymlinks:true. pnpm puts every dep under
      // .pnpm/<pkg>@<ver>/node_modules/<pkg> and exposes a symlink at
      // node_modules/<pkg>. With preserveSymlinks @rollup/plugin-commonjs
      // mis-resolves intra-package requires (e.g. ajv's require of its own
      // ./codegen) and emits a stub external `require$$N` that crashes at
      // runtime. The symlinked ui/src/gen dir is handled below by aliasing
      // the importer side, not by preserving symlinks globally.
      alias: [],
    },
    define: {
      // Immer reads process.env.NODE_ENV; not defined in browser.
      'process.env.NODE_ENV': JSON.stringify('production'),
    },
    // The wasm glue files in ui/src/gen (proto_utils.js, trace_processor.js,
    // traceconv.js) are UMD with a tacked-on `module.exports.default`. Vite's
    // built-in CJS handling needs help recognising them as CJS so the
    // `import X from '../gen/foo'` pattern works.
    optimizeDeps: {
      esbuildOptions: {
        // No-op for build, but keeps dev parity if we ever flip to `vite dev`.
      },
    },
    build: isBuild
      ? {
          commonjsOptions: {
            transformMixedEsModules: true,
            // The UMD wasm glue files don't look like CJS to rollup's auto-detect.
            // Force CJS interpretation for everything under ui/src/gen/*.js.
            include: [/node_modules/, /\/gen\/.*\.js$/],
            // ajv's JIT validator codegen emits strings like
            //   `require("ajv/dist/runtime/equal").default`
            // which @rollup/plugin-commonjs tries to resolve as transitive deps and
            // then leaves dangling `require$$N` references in the IIFE. Those
            // require()s are only ever evaluated inside ajv's own codegen layer,
            // never at runtime in the browser. Tell commonjs not to analyse them.
            ignoreDynamicRequires: true,
          },
          outDir: path.join(OUT_SYMLINK, bundleCfg.dir),
          emptyOutDir: false, // build.mjs puts wasm/css/assets here too.
          // Force a single CSS asset per bundle (named after the entry chunk, e.g.
          // frontend.css). IIFE builds don't auto-inject <link> tags, so extraction
          // needs to be explicit.
          cssCodeSplit: false,
          sourcemap: !NO_SOURCE_MAPS,
          minify: MINIFY_JS ? 'terser' : false,
          terserOptions:
            MINIFY_JS === 'preserve_comments'
              ? {format: {comments: 'all'}}
              : undefined,
          rollupOptions: {
            input: {[BUNDLE]: inputPath},
            treeshake: NO_TREESHAKE ? false : undefined,
            output: {
              format: 'iife',
              name: BUNDLE,
              entryFileNames,
              // With cssCodeSplit:false Vite emits the CSS as "style.css" by
              // default. Rename it to <bundle>.css so that index.html's preload
              // and the assetSrc('frontend.css') call match.
              assetFileNames: (info) => {
                const name = info.names?.[0] || info.name || '';
                if (name.endsWith('.css')) return `${BUNDLE}.css`;
                return '[name][extname]';
              },
              inlineDynamicImports: true,
            },
            onwarn(warning, warn) {
              if (warning.code === 'CIRCULAR_DEPENDENCY') {
                if ((warning.message || '').includes('node_modules')) return;
                throw new Error(
                  `Circular dependency: ${warning.importer}\n  ${(warning.cycle || []).join('\n  ')}`,
                );
              }
              warn(warning);
            },
          },
        }
      : undefined,
  };
});
