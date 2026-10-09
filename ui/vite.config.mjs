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
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {SourceMapConsumer, SourceMapGenerator} from 'source-map';
import {lezer} from '@lezer/generator/rollup';

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
const EXPERIMENTAL_MITHRIL_HMR =
  process.env.EXPERIMENTAL_MITHRIL_HMR === 'true';

// Unlike Rollup, Rolldown does not polyfill import.meta.url in IIFE bundles.
// Some dependencies use it at module initialization time, so provide the
// browser equivalent explicitly. This works both in documents and workers.
const IMPORT_META_URL = '__perfetto_import_meta_url__';
const IMPORT_META_URL_INTRO = `
var _documentCurrentScript =
  typeof document !== 'undefined' ? document.currentScript : null;
var ${IMPORT_META_URL} =
  typeof document === 'undefined'
    ? location.href
    : (_documentCurrentScript &&
       _documentCurrentScript.tagName.toUpperCase() === 'SCRIPT' &&
       _documentCurrentScript.src) || document.baseURI;
`;

// IIFE bundles go to dist_version (the symlink to dist/v1.2.3). The service
// worker and chrome extension go elsewhere; for the minimum migration we keep
// them on the old rollup path until needed. (build.mjs still runs rollup for
// those if we wire it that way, but for now we ship the three main bundles.)
const SRC = path.join(ROOT_DIR, 'ui/src');
const GEN_SYMLINK = path.join(SRC, 'gen');
// Canonical (real) path of the gen dir. Vite/Rollup resolves symlinks by
// default, so a file imported through ui/src/gen ends up reported with this
// path. We use it below to detect "the importer is a generated file".
const GEN_REAL = fs.existsSync(GEN_SYMLINK) ? fs.realpathSync(GEN_SYMLINK) : '';

// The generated import barrels (all_plugins.ts, all_core_plugins.ts) live under
// ui/src/gen which is a symlink into out/.../tsc/gen. Their relative imports
// (e.g. "../core_plugins/foo") only make sense relative to ui/src, not to the
// real gen dir. When Rollup canonicalises the symlink, those relative paths
// break. This plugin rewrites them to absolute paths under ui/src.
// Strips Rollup's full source map down to one mapping per generated line and
// embeds it inline into each _bundle.js under self.__SOURCEMAPS[fileName] for
// runtime error reporting. Skipped when source maps are disabled.
function pluginEmbedMinimalSourceMap() {
  return {
    name: 'perfetto:embed-minimal-sourcemap',
    async generateBundle(_options, bundle) {
      for (const chunk of Object.values(bundle)) {
        if (!chunk.fileName || !chunk.fileName.endsWith('.js') || !chunk.map) {
          continue;
        }
        try {
          const consumer = await new SourceMapConsumer(chunk.map);
          const generator = new SourceMapGenerator({file: chunk.map.file});
          const seenLines = new Set();
          const cleanSourcePath = (source) => {
            let cleaned = source.replace('../../../out/ui/', '');
            cleaned = cleaned.replace('../../node_modules/', 'node_modules/');
            return cleaned;
          };
          consumer.eachMapping((mapping) => {
            if (!mapping.source) return;
            if (seenLines.has(mapping.generatedLine)) return;
            seenLines.add(mapping.generatedLine);
            generator.addMapping({
              generated: {line: mapping.generatedLine, column: 0},
              original: {
                line: mapping.originalLine,
                column: mapping.originalColumn,
              },
              source: cleanSourcePath(mapping.source),
            });
          });
          consumer.destroy();
          const minimalMap = JSON.parse(generator.toString());
          delete minimalMap.sourcesContent;
          delete minimalMap.names;
          chunk.code +=
            `\n;(self.__SOURCEMAPS=self.__SOURCEMAPS||{})` +
            `['${chunk.fileName}']=${JSON.stringify(minimalMap)};`;
        } catch (err) {
          console.error(
            `Error creating minimal source map for ${chunk.fileName}:`,
            err.message,
          );
        }
      }
    },
  };
}

// Shared helper for plugins that synthesise a module's source on the fly but
// expose it via a normal relative import (typed by a colocated .d.ts).
//
// `modules` maps an absolute path (no extension) — e.g. <SRC>/plugins/index —
// to a function that returns the module source. resolveId intercepts both
// file-style imports ('../base/version') and directory-style imports
// ('../plugins' → '../plugins/index') before Vite's filesystem resolver runs.
function makeSynthModulePlugin({name, modules}) {
  const PREFIX = '\0' + name + ':';
  return {
    name,
    enforce: 'pre',
    resolveId(source, importer) {
      if (!importer || !source.startsWith('.')) return null;
      const stripped = source.replace(/\.(ts|js)$/, '');
      const abs = path.resolve(path.dirname(importer), stripped);
      for (const candidate of [abs, path.join(abs, 'index')]) {
        if (!(candidate in modules)) continue;
        // A real implementation next to the .d.ts would be silently shadowed
        // by this plugin. Fail loudly instead.
        for (const ext of ['.ts', '.js']) {
          if (fs.existsSync(candidate + ext)) {
            throw new Error(
              `${path.relative(ROOT_DIR, candidate + ext)} shadows a ` +
                `synthesised module (see ${name} in ui/vite.config.mjs); ` +
                `delete the file.`,
            );
          }
        }
        return PREFIX + candidate;
      }
    },
    load(id) {
      if (!id.startsWith(PREFIX)) return;
      const gen = modules[id.slice(PREFIX.length)];
      if (gen) return gen(this);
    },
  };
}

// The plugin barrels (ui/src/virtual/plugins.ts) are no longer synthesised
// here: that module now uses Vite's import.meta.glob directly, which discovers
// plugin dirs and re-evaluates on add/remove without manual file-watching.

// Exposes VERSION and SCM_REVISION via ui/src/virtual/version (typed by
// version.d.ts). Replaces the on-disk ui/src/gen/perfetto_version.ts that
// build.mjs used to generate via tools/write_version_header.py.
export function pluginPerfettoVersion() {
  const SCRIPT = path.join(ROOT_DIR, 'tools/write_version_header.py');
  const generate = () => {
    const out = execFileSync('python3', [SCRIPT, '--json'], {encoding: 'utf8'});
    const {version, sha1} = JSON.parse(out);
    return (
      `export const VERSION = ${JSON.stringify(version)};\n` +
      `export const SCM_REVISION = ${JSON.stringify(sha1)};\n`
    );
  };
  return makeSynthModulePlugin({
    name: 'perfetto:version',
    modules: {[path.join(SRC, 'virtual', 'version')]: generate},
  });
}

function pluginGenRelativeImports() {
  return {
    name: 'perfetto:gen-relative-imports',
    enforce: 'pre',
    async resolveId(source, importer) {
      if (!importer || !source.startsWith('.')) return null;
      if (!GEN_REAL || !importer.startsWith(GEN_REAL + path.sep)) return null;
      // Re-anchor the relative import to ui/src/gen (the symlinked location)
      // rather than the canonical out/.../tsc/gen path.
      const reanchored = path.resolve(GEN_SYMLINK, source);
      return this.resolve(reanchored, importer, {skipSelf: true});
    },
  };
}

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

// Dev-mode hot module replacement for Mithril components.
//
// A "component module" is a ui/src .ts file whose only runtime exports are
// Mithril components (type-only exports are allowed). Components are
// identified by an explicit type annotation:
//      export class Foo implements m.ClassComponent<...> {...}
//      export class Foo implements m.Component<...> {...}
//      export function Foo(...): m.Component<...> {...}
//      export const Foo: m.ClosureComponent<...> = ...;
//      export const Foo: m.FactoryComponent<...> = ...;
//      export const Foo: m.Component<...> = {...};
// Such modules are rewritten so each exported component is wrapped by
// hmrWrapComponent() (ui/src/core/hmr.ts) and the module self-accepts. On
// update the runtime repoints the wrapper at the new component and remounts
// the whole Mithril tree.
//
// Any other JS/TS change triggers a full page reload (which the
// vite_live_reload.ts prompt intercepts). This is deliberate: letting Vite
// propagate a non-component update up to a self-accepting component module
// would re-execute only part of the graph and leave the app with two copies
// of the changed module.
//
// The rewrite preserves line/column positions of the original code (the
// `export` keyword is blanked, everything else is appended on the same line
// or at the end of the file), so no source map is needed.
//
// NB: the wrapped export is a `const`, so exported function declarations lose
// hoisting from the importer's point of view. This only matters for import
// cycles that use a component before its module has finished evaluating.
function pluginMithrilHmr() {
  const HMR_RUNTIME = path.join(SRC, 'core/hmr.ts');
  let tsPromise;
  const loadTs = () =>
    (tsPromise ??= import('typescript').then((m) => m.default));

  // Files that were served as component modules (i.e. currently self-accept).
  const componentModules = new Set();

  const hasModifier = (ts, node, kind) =>
    (node.modifiers ?? []).some((m) => m.kind === kind);

  // True if `typeNode` is a reference to one of `names` (e.g. m.Component<A>).
  const isTypeRef = (ts, sf, typeNode, names) =>
    !!typeNode &&
    ts.isTypeReferenceNode(typeNode) &&
    names.includes(typeNode.typeName.getText(sf));

  // If `stmt` is an exported component, returns its name, otherwise null.
  function componentName(ts, sf, stmt) {
    if (
      hasModifier(ts, stmt, ts.SyntaxKind.DefaultKeyword) ||
      hasModifier(ts, stmt, ts.SyntaxKind.DeclareKeyword)
    ) {
      return null;
    }
    if (ts.isClassDeclaration(stmt)) {
      const implementsComponent = (stmt.heritageClauses ?? []).some(
        (clause) =>
          clause.token === ts.SyntaxKind.ImplementsKeyword &&
          clause.types.some((t) =>
            ['m.ClassComponent', 'm.Component'].includes(
              t.expression.getText(sf),
            ),
          ),
      );
      return stmt.name && implementsComponent ? stmt.name.text : null;
    }
    if (ts.isFunctionDeclaration(stmt)) {
      const ok =
        stmt.name &&
        stmt.body &&
        !stmt.asteriskToken &&
        !hasModifier(ts, stmt, ts.SyntaxKind.AsyncKeyword) &&
        isTypeRef(ts, sf, stmt.type, ['m.Component']);
      return ok ? stmt.name.text : null;
    }
    if (ts.isVariableStatement(stmt)) {
      const list = stmt.declarationList;
      if (!(list.flags & ts.NodeFlags.Const)) return null;
      if (list.declarations.length !== 1) return null;
      const decl = list.declarations[0];
      if (!ts.isIdentifier(decl.name) || !decl.initializer) return null;
      const init = decl.initializer;
      const isClosure =
        (ts.isArrowFunction(init) || ts.isFunctionExpression(init)) &&
        isTypeRef(ts, sf, decl.type, [
          'm.ClosureComponent',
          'm.FactoryComponent',
        ]);
      const isObject =
        ts.isObjectLiteralExpression(init) &&
        isTypeRef(ts, sf, decl.type, ['m.Component']);
      return isClosure || isObject ? decl.name.text : null;
    }
    return null;
  }

  // Returns the list of exported components, or null if the module is not a
  // component module.
  function analyze(ts, file, code) {
    const sf = ts.createSourceFile(file, code, ts.ScriptTarget.Latest, true);
    const components = [];
    for (const stmt of sf.statements) {
      if (ts.isExportDeclaration(stmt)) {
        const typeOnly =
          stmt.isTypeOnly ||
          (stmt.exportClause &&
            ts.isNamedExports(stmt.exportClause) &&
            !stmt.moduleSpecifier &&
            stmt.exportClause.elements.every((e) => e.isTypeOnly));
        if (!typeOnly) return null;
        continue;
      }
      if (ts.isExportAssignment(stmt)) return null;
      if (!hasModifier(ts, stmt, ts.SyntaxKind.ExportKeyword)) continue;
      if (ts.isInterfaceDeclaration(stmt) || ts.isTypeAliasDeclaration(stmt)) {
        continue;
      }
      const name = componentName(ts, sf, stmt);
      if (name === null) return null;
      const exportKw = stmt.modifiers.find(
        (m) => m.kind === ts.SyntaxKind.ExportKeyword,
      );
      components.push({
        name,
        exportStart: exportKw.getStart(sf),
        exportEnd: exportKw.getEnd(),
        declEnd: stmt.getEnd(),
      });
    }
    return components.length > 0 ? components : null;
  }

  return {
    name: 'perfetto:mithril-hmr',
    apply: 'serve',
    enforce: 'pre',
    async transform(code, id) {
      const file = id.split('?', 1)[0];
      if (!file.startsWith(SRC + path.sep) || !file.endsWith('.ts')) return;
      if (file.endsWith('.d.ts') || file === HMR_RUNTIME) return;
      // Cheap pre-filter before parsing.
      if (!/\bexport\s+(abstract\s+class|class|function|const)\b/.test(code)) {
        componentModules.delete(file);
        return;
      }
      const components = analyze(await loadTs(), file, code);
      if (!components) {
        componentModules.delete(file);
        return;
      }
      componentModules.add(file);

      // Apply edits back-to-front so earlier offsets stay valid.
      let out = code;
      for (const c of [...components].reverse()) {
        const local = `__hmr_${c.name}`;
        out =
          out.slice(0, c.declEnd) +
          `; const ${local} = __hmrWrapComponent(import.meta.hot, ` +
          `${JSON.stringify(c.name)}, ${c.name}); ` +
          `export {${local} as ${c.name}};` +
          out.slice(c.declEnd);
        out =
          out.slice(0, c.exportStart) +
          ' '.repeat(c.exportEnd - c.exportStart) +
          out.slice(c.exportEnd);
      }
      let rel = path
        .relative(path.dirname(file), HMR_RUNTIME)
        .replace(/\.ts$/, '')
        .split(path.sep)
        .join('/');
      if (!rel.startsWith('.')) rel = './' + rel;
      // ESM imports are hoisted, so appending it at the end is fine.
      out +=
        `\nimport {hmrWrapComponent as __hmrWrapComponent} from ` +
        `${JSON.stringify(rel)};\n` +
        `if (import.meta.hot) {\n  import.meta.hot.accept();\n}\n`;
      return {code: out, map: null};
    },
    async hotUpdate({file, modules, read}) {
      if (this.environment.name !== 'client') return;
      if (modules.length === 0) return; // Not part of the module graph.
      if (/\.(css|scss|sass)$/.test(file)) return; // Vite hot-swaps styles.
      // Only take the HMR path if the module is a component module both
      // before (so it currently self-accepts) and after the edit.
      if (componentModules.has(file) && file.endsWith('.ts')) {
        if (analyze(await loadTs(), file, await read())) return;
      }
      this.environment.hot.send({
        type: 'full-reload',
        path: '*',
        triggeredBy: file,
      });
      return [];
    },
  };
}

// Per-bundle config: input file, output dir (relative to ui/out), and output
// filename. Most bundles follow the standard convention; service_worker and
// chrome_extension differ.
const BUNDLE_CONFIGS = {
  frontend: {dir: 'dist_version', entry: 'index.ts'},
  engine: {dir: 'dist_version', entry: 'index.ts'},
  engine_bench: {dir: 'dist_version', entry: 'index.ts'},
  engine_bench_worker: {
    dir: 'dist_version',
    srcDir: 'engine_bench',
    entry: 'worker.ts',
  },
  traceconv: {dir: 'dist_version', entry: 'index.ts'},
  bigtrace: {dir: 'dist_version/bigtrace', entry: 'index.ts'},
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
  const inputPath = isBuild
    ? path.join(SRC, bundleCfg.srcDir ?? BUNDLE, bundleCfg.entry)
    : null;
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
      pluginPerfettoVersion(),
      // Compiles *.grammar files (lezer parser definitions) on import. Replaces
      // the old "manually run lezer-generator and commit gen/*.js" workflow.
      lezer(),
      pluginGenRelativeImports(),
      ...(isBuild ? [] : [pluginGenWasmGlueEsm()]),
      ...(!isBuild && EXPERIMENTAL_MITHRIL_HMR ? [pluginMithrilHmr()] : []),
      ...(NO_SOURCE_MAPS ? [] : [pluginEmbedMinimalSourceMap()]),
    ],
    resolve: {
      // NB: do NOT set preserveSymlinks:true. pnpm puts every dep under
      // .pnpm/<pkg>@<ver>/node_modules/<pkg> and exposes a symlink at
      // node_modules/<pkg>. With preserveSymlinks @rollup/plugin-commonjs
      // mis-resolves intra-package requires (e.g. ajv's require of its own
      // ./codegen) and emits a stub external `require$$N` that crashes at
      // runtime. The symlinked ui/src/gen dir is handled below by aliasing
      // the importer side, not by preserving symlinks globally.
      alias: [
        // The trace_processor_32_stub indirection (see old rollup.config.js).
        ...(IS_MEMORY64_ONLY
          ? []
          : [
              {
                find: /.*\/trace_processor_32_stub$/,
                replacement: path.join(SRC, 'gen/trace_processor'),
              },
            ]),
      ],
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
          rolldownOptions: {
            input: {[BUNDLE]: inputPath},
            treeshake: NO_TREESHAKE ? false : undefined,
            transform: {
              define: {'import.meta.url': IMPORT_META_URL},
            },
            output: {
              format: 'iife',
              name: BUNDLE,
              entryFileNames,
              intro: IMPORT_META_URL_INTRO,
              // With cssCodeSplit:false Vite emits the CSS as "style.css" by
              // default. Rename it to <bundle>.css so that index.html's preload
              // and the assetSrc('frontend.css') call match.
              assetFileNames: (info) => {
                const name = info.names?.[0] || info.name || '';
                if (name.endsWith('.css')) return `${BUNDLE}.css`;
                return '[name][extname]';
              },
            },
            onwarn(warning, warn) {
              if (warning.code === 'CIRCULAR_DEPENDENCY') {
                if ((warning.message || '').includes('node_modules')) return;
                // Rollup >=4 reports the cycle in `warning.ids` (older versions
                // used `warning.importer`/`warning.cycle`, which are now
                // undefined). `warning.message` already contains a formatted
                // "a -> b -> a" chain, so prefer it and fall back to `ids`.
                const cycle = warning.ids ?? warning.cycle ?? [];
                throw new Error(
                  `${warning.message ?? 'Circular dependency'}\n  ${cycle.join('\n  ')}`,
                );
              }
              warn(warning);
            },
          },
        }
      : undefined,
  };
});
