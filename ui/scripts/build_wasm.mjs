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

// Invokes tools/gn + tools/ninja to build the UI wasm modules and stages
// outputs into the locations consumed by the Vite build:
//   - <outDir>/wasm[_memory64]/<mod>.wasm    -> <outDistDir>/<mod>.wasm
//   - <outDir>/wasm[_memory64]/<mod>.js      -> <outGenDir>/<mod>.js
//   - <outDir>/wasm[_memory64]/<mod>.d.ts    -> <outGenDir>/<mod>.d.ts
//
// Options:
//   - `skipBuild=true` skips gn+ninja and just re-stages existing outputs.
//   - `onlyMemory64=true` drops the 32-bit trace_processor target.
//   - `noOverrideGnArgs=true` does not run `gn gen`; expects the caller to
//     have prepared the build dir.
//   - `debug=true` sets is_debug=true when (re)generating gn args.

import {join} from 'node:path';
import fs from 'node:fs';
import {
  ROOT_DIR,
  cp,
  ensureDir,
  exec,
  execCapture,
  fileExists,
} from './common.mjs';

export function wasmModules({onlyMemory64 = false} = {}) {
  const mods = ['traceconv', 'proto_utils', 'trace_processor_memory64'];
  if (!onlyMemory64) mods.push('trace_processor');
  return mods;
}

function isMemory64(mod) {
  return mod.endsWith('_memory64');
}

function wasmOutDirFor(layout, mod) {
  return isMemory64(mod) ? layout.outWasmMemory64Dir : layout.outWasmDir;
}

// Verifies whether the existing gn args match what we would set. If not,
// re-runs `gn gen` with the correct args.
function ensureGnArgs(layout, {debug = false}) {
  const argsFile = join(layout.outDir, 'args.gn');
  let current = '';
  if (fileExists(argsFile)) {
    current = fs.readFileSync(argsFile, 'utf8');
  }
  let wanted = `is_debug=${debug}`;
  try {
    const res = execCapture('which', ['ccache']);
    if (res.trim().length > 0) wanted += `\ncc_wrapper="ccache"`;
  } catch {
    // ccache not installed, skip.
  }
  if (current.trim() === wanted.trim()) return;
  ensureDir(layout.outDir);
  exec(join(ROOT_DIR, 'tools/gn'), [
    'gen',
    `--args=${wanted.replace(/\n/g, ' ')}`,
    layout.outDir,
  ]);
}

// Runs ninja to build the wasm modules, then stages outputs.
export function buildWasm(
  layout,
  {onlyMemory64 = false, noOverrideGnArgs = false, debug = false, skipBuild = false} = {},
) {
  const mods = wasmModules({onlyMemory64});

  if (!skipBuild) {
    if (!noOverrideGnArgs) {
      ensureGnArgs(layout, {debug});
    }
    const ninjaArgs = ['-C', layout.outDir, ...mods.map((m) => `${m}_wasm`)];
    exec(join(ROOT_DIR, 'tools/ninja'), ninjaArgs);
  }

  stageWasm(layout, {onlyMemory64, debug});
}

// Copies already-built wasm outputs into the gen dir and dist dir. Idempotent.
export function stageWasm(
  layout,
  {onlyMemory64 = false, debug = false} = {},
) {
  const mods = wasmModules({onlyMemory64});
  ensureDir(layout.outDistDir);
  ensureDir(layout.outGenDir);
  for (const mod of mods) {
    const wasmOutDir = wasmOutDirFor(layout, mod);
    const exts = ['.wasm'];
    if (debug) exts.push('.wasm.map');
    for (const ext of exts) {
      const src = `${wasmOutDir}/${mod}${ext}`;
      if (fileExists(src)) cp(src, join(layout.outDistDir, `${mod}${ext}`));
    }
    for (const ext of ['.js', '.d.ts']) {
      const src = `${wasmOutDir}/${mod}${ext}`;
      if (fileExists(src)) cp(src, join(layout.outGenDir, `${mod}${ext}`));
    }
  }

  // When only building memory64, there is no 32-bit trace_processor glue.
  // Vite's resolver would crash on `import ... from '../gen/trace_processor'`.
  // Under the old rollup setup this import sat behind a `trace_processor_32_stub`
  // that got rewritten by rollup-plugin-re only when the 32-bit build was
  // enabled. Under Vite we use an aliasing plugin to redirect the stub to the
  // real module when present. No placeholder is needed on disk.
}

// CLI entry point.
if (import.meta.url === `file://${process.argv[1]}`) {
  const {makeLayout, resolveOutDir, readVersion} = await import(
    './common.mjs'
  );
  const argv = process.argv.slice(2);
  const onlyMemory64 = argv.includes('--only-memory64');
  const noBuild = argv.includes('--no-build');
  const debug = argv.includes('--debug');
  const noOverrideGnArgs = argv.includes('--no-override-gn-args');
  const layout = makeLayout(resolveOutDir(), readVersion());
  buildWasm(layout, {
    onlyMemory64,
    skipBuild: noBuild,
    debug,
    noOverrideGnArgs,
  });
}
