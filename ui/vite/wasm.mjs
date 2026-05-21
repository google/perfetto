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

import path from 'node:path';

// Rewrites `import X from '<virtualDir>/<name>'` to the real emscripten glue
// under |genDir| at resolve time. TypeScript sees only the manually-curated
// .d.ts in the virtual dir, which avoids pulling the giant generated .js into
// the type system.
//
// Under |isMemory64Only| the 32-bit trace_processor entry resolves to a
// virtual stub module whose default export throws — keeps the build linkable
// without shipping a real stub file.
export function pluginPerfettoVirtualWasmModules({
  virtualDir,
  genDir,
  isMemory64Only = false,
}) {
  const TP32_STUB_ID = '\0perfetto:trace-processor-32-stub';
  const TP32_STUB_SOURCE =
    `export default () => {\n` +
    `  throw new Error(\n` +
    `    'Unable to load the 32-bit trace_processor.wasm. This browser ' +\n` +
    `    'does NOT support Memory64 but --only-wasm-memory64 was passed ' +\n` +
    `    'to ui/build.'\n` +
    `  );\n` +
    `};\n`;
  const TARGETS = {
    [path.join(virtualDir, 'trace_processor')]: isMemory64Only
      ? TP32_STUB_ID
      : path.join(genDir, 'trace_processor.js'),
    [path.join(virtualDir, 'trace_processor_memory64')]: path.join(
      genDir,
      'trace_processor_memory64.js',
    ),
    [path.join(virtualDir, 'proto_utils')]: path.join(
      genDir,
      'proto_utils.js',
    ),
    [path.join(virtualDir, 'traceconv')]: path.join(genDir, 'traceconv.js'),
  };
  return {
    name: 'perfetto:virtual-wasm-modules',
    enforce: 'pre',
    async resolveId(source, importer) {
      if (!importer || !source.startsWith('.')) return null;
      const stripped = source.replace(/\.(ts|js)$/, '');
      const abs = path.resolve(path.dirname(importer), stripped);
      const target = TARGETS[abs];
      if (!target) return null;
      if (target === TP32_STUB_ID) return TP32_STUB_ID;
      return this.resolve(target, importer, {skipSelf: true});
    },
    load(id) {
      if (id === TP32_STUB_ID) return TP32_STUB_SOURCE;
    },
  };
}
