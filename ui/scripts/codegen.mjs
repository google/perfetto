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

// Runs all codegen steps that produce files consumed by the Vite build:
//   1. pbjs + pbts -> ui/src/gen/protos.{js,d.ts}
//   2. tools/write_version_header.py --ts_out -> ui/src/gen/perfetto_version.ts
//   3. tools/gen_ui_imports -> ui/src/gen/all_{core_,}plugins.{ts,scss}
//   4. tools/gen_stdlib_docs_json.py -> dist/<ver>/stdlib_docs.json

import {join} from 'node:path';
import {
  UI_DIR,
  ROOT_DIR,
  ensureDir,
  ensureSymlinks,
  exec,
  walk,
} from './common.mjs';

const PROTO_INPUTS = [
  'protos/perfetto/ipc/consumer_port.proto',
  'protos/perfetto/ipc/wire_protocol.proto',
  'protos/perfetto/trace/perfetto/perfetto_metatrace.proto',
  'protos/perfetto/perfetto_sql/structured_query.proto',
  'protos/perfetto/trace_processor/trace_processor.proto',
];

function pbjsBin() {
  return join(UI_DIR, 'node_modules/.bin/pbjs');
}

function pbtsBin() {
  return join(UI_DIR, 'node_modules/.bin/pbts');
}

export function generateProtos(layout) {
  const dstJs = join(layout.outGenDir, 'protos.js');
  const dstTs = join(layout.outGenDir, 'protos.d.ts');
  ensureDir(layout.outGenDir);

  // Comments are load-bearing here: pbts reads them from |dstJs|.
  // We use -w es6 so that Vite's dev server can serve the generated file as
  // a native ES module from /src/gen/protos.js (Vite only pre-bundles CJS
  // from node_modules, not from the source tree).
  const pbjsArgs = [
    '--no-beautify',
    '--force-number',
    '--no-delimited',
    '--no-verify',
    '-t',
    'static-module',
    '-w',
    'es6',
    '-p',
    ROOT_DIR,
    '-o',
    dstJs,
    ...PROTO_INPUTS,
  ];
  exec(pbjsBin(), pbjsArgs);

  const pbtsArgs = ['--no-comments', '-p', ROOT_DIR, '-o', dstTs, dstJs];
  exec(pbtsBin(), pbtsArgs);
}

export function generateVersion(layout) {
  ensureDir(layout.outGenDir);
  exec('python3', [
    join(ROOT_DIR, 'tools/write_version_header.py'),
    '--ts_out',
    join(layout.outGenDir, 'perfetto_version.ts'),
  ]);
}

export function generatePluginImports() {
  // Writes both .ts (imports) and .scss (@import blocks) alongside.
  const genDir = join(UI_DIR, 'src/gen');
  ensureDir(genDir);
  const tool = join(ROOT_DIR, 'tools/gen_ui_imports');
  exec('python3', [
    tool,
    join(UI_DIR, 'src/core_plugins'),
    '--out',
    join(genDir, 'all_core_plugins'),
  ]);
  exec('python3', [
    tool,
    join(UI_DIR, 'src/plugins'),
    '--out',
    join(genDir, 'all_plugins'),
  ]);
}

export function generateStdlibDocs(layout) {
  ensureDir(layout.outDistDir);
  const stdlibDir = join(ROOT_DIR, 'src/trace_processor/perfetto_sql/stdlib');
  const sqlFiles = [];
  walk(stdlibDir, (p) => {
    if (p.endsWith('.sql')) sqlFiles.push(p);
  });
  exec('python3', [
    join(ROOT_DIR, 'tools/gen_stdlib_docs_json.py'),
    '--json-out',
    join(layout.outDistDir, 'stdlib_docs.json'),
    '--minify',
    ...sqlFiles,
  ]);
}

export function runAll(layout) {
  ensureSymlinks(layout);
  generateProtos(layout);
  generateVersion(layout);
  generatePluginImports();
  generateStdlibDocs(layout);
}

// CLI entry point for standalone invocation.
if (import.meta.url === `file://${process.argv[1]}`) {
  const {makeLayout, resolveOutDir, readVersion} = await import(
    './common.mjs'
  );
  const outDir = resolveOutDir();
  const version = readVersion();
  const layout = makeLayout(outDir, version);
  runAll(layout);
}
