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

// Custom Vite plugins that replicate behaviors of the old rollup.config.js.

import fs from 'node:fs';
import {dirname, join, isAbsolute} from 'node:path';

// Replicates the rollup-plugin-re hacks from the old rollup.config.js:
// - Protobufjs's inquire() uses eval(moduleName) to dynamically require
//   modules. In the browser this is never needed and it makes bundlers emit
//   warnings / fail to tree-shake. Rewrite it to `undefined;` so the import
//   is inert.
// - Immer's entry point branches on `process.env.NODE_ENV` but `process`
//   isn't defined in the browser. We replace the whole expression with the
//   string literal 'production'. (Note: Vite's `define` option only works
//   when the reference is statically analyzable, and some transform pipelines
//   inline the check earlier than `define` fires, so a textual replace is
//   the most robust workaround.)
export function pluginProtoFixup() {
  return {
    name: 'perfetto:proto-fixup',
    enforce: 'pre',
    transform(code, id) {
      // Only rewrite .js files; .ts files have no matching patterns.
      if (!id.endsWith('.js')) return null;
      let out = code;
      let changed = false;
      // Broad enough to hit both the protobufjs source-tree copy and the
      // pbjs-generated protos.js.
      if (/eval\(.*\(moduleName\);/.test(out)) {
        out = out.replace(/eval\(.*\(moduleName\);/g, 'undefined;');
        changed = true;
      }
      if (/process\.env\.NODE_ENV/.test(out)) {
        out = out.replace(/process\.env\.NODE_ENV/g, "'production'");
        changed = true;
      }
      return changed ? {code: out, map: null} : null;
    },
  };
}

// The Emscripten `-s MODULARIZE=1` glue we build for the wasm modules ends
// with a CommonJS/AMD trailer:
//
//   var <mod>_wasm = (() => { ... })();
//   if (typeof exports === 'object' && typeof module === 'object') {
//     module.exports = <mod>_wasm;
//     module.exports.default = <mod>_wasm;
//   } else if (typeof define === 'function' && define['amd'])
//     define([], () => <mod>_wasm);
//
// In a production bundle, @rollup/plugin-commonjs converts that into a
// proper ES default export. In Vite's dev server the file is served
// verbatim to the browser as an ES module — so neither `module.exports`
// nor AMD are satisfied and the module ends up with no default export,
// breaking `import TraceProcessor from '../gen/trace_processor_memory64'`.
//
// This plugin runs only in dev-serve mode. It pattern-matches the
// `var <name>_wasm = (...)` opener and appends `export default <name>_wasm;`
// to the file. The existing CJS/AMD trailer becomes a harmless dead branch.
export function pluginEmscriptenGlueToEsm() {
  return {
    name: 'perfetto:emscripten-glue-esm',
    apply: 'serve',
    enforce: 'pre',
    transform(code, id) {
      if (!id.endsWith('.js')) return null;
      if (!/\/src\/gen\/[^/]+\.js$/.test(id)) return null;
      const m = code.match(/^var (\w+_wasm) = \(/m);
      if (!m) return null;
      return {
        code: `${code}\nexport default ${m[1]};\n`,
        map: null,
      };
    },
  };
}

// Lezer grammars are generated into `<name>.grammar.js` with a sibling
// `<name>.grammar.terms.js`. The sources import via the bare-extension form
// `./foo.grammar`, which used to resolve to the `.grammar.js` file under
// rollup-node-resolve's extension probing. Vite's resolver prefers the exact
// match (i.e. the raw grammar text file) and then errors on parsing it as JS.
// This plugin rewrites `.grammar` specifiers to `.grammar.js` explicitly.
export function pluginLezerGrammarAlias() {
  return {
    name: 'perfetto:lezer-grammar-alias',
    enforce: 'pre',
    async resolveId(source, importer) {
      if (!source.endsWith('.grammar')) return null;
      const rewritten = source + '.js';
      return this.resolve(rewritten, importer, {skipSelf: true});
    },
  };
}

// When the 32-bit trace_processor wasm has not been built (e.g. because the
// user passed --only-wasm-memory64), the source file
//   ui/src/engine/trace_processor_32_stub.ts
// throws on use. When the 32-bit build IS present, we rewrite the relative
// import in ui/src/engine/wasm_bridge.ts from './trace_processor_32_stub' to
// '../gen/trace_processor' so the real Emscripten glue is bundled in.
//
// The old rollup config used rollup-plugin-re for this; we do the same but
// via resolveId so there's no risk of matching unrelated strings.
export function pluginTraceProcessor32Alias({genDir}) {
  const realGluePath = join(genDir, 'trace_processor.js');
  return {
    name: 'perfetto:trace-processor-32-alias',
    async resolveId(source, importer) {
      if (source !== './trace_processor_32_stub') return null;
      // Only rewrite when the real module is present on disk.
      if (!fs.existsSync(realGluePath)) return null;
      // Resolve using the usual mechanism against the gen dir so that the
      // chain picks up the .d.ts pair too.
      const resolved = await this.resolve(join(genDir, 'trace_processor'), importer, {
        skipSelf: true,
      });
      return resolved;
    },
  };
}

// Keep hashed + deterministic file names out, enforce `<name>_bundle.js` on
// entry and a stable shape for non-entry assets. Used as an output hook.
export function entryFileNamesNoHash(bundleName) {
  return `${bundleName}_bundle.js`;
}
