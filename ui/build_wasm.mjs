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

// Wasm build steps invoked from build.mjs:
//   - buildWasm():               runs gn+ninja for trace_processor / traceconv
//                                / proto_utils Wasm modules and stages their
//                                .wasm/.js/.d.ts outputs.
//   - copySyntaqliteRuntime():   copies the prebuilt syntaqlite runtime out of
//                                node_modules and compiles our SQL dialect
//                                side module via emcc.

import childProcess from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const pjoin = path.join;

// |ctx| must provide: ROOT_DIR, addTask, exec, cp, ensureDir.
// |cfg| must provide: outDir, outDistDir, outDistRootDir, outGenDir, debug,
// noOverrideGnArgs, wasmModules.

export function buildWasm(ctx, cfg, skipWasmBuild) {
  const {ROOT_DIR, addTask, exec, cp} = ctx;

  if (!skipWasmBuild) {
    if (!cfg.noOverrideGnArgs) {
      let gnVars = `is_debug=${cfg.debug}`;
      if (childProcess.spawnSync('which', ['ccache']).status === 0) {
        gnVars += ` cc_wrapper="ccache"`;
      }
      const gnArgs = ['gen', `--args=${gnVars}`, cfg.outDir];
      addTask(exec, [pjoin(ROOT_DIR, 'tools/gn'), gnArgs]);
    }
    const ninjaArgs = ['-C', cfg.outDir];
    ninjaArgs.push(...cfg.wasmModules.map((x) => `${x}_wasm`));
    addTask(exec, [pjoin(ROOT_DIR, 'tools/ninja'), ninjaArgs]);
  }

  for (const wasmMod of cfg.wasmModules) {
    const isMem64 = wasmMod.endsWith('_memory64');
    const wasmOutDir = pjoin(cfg.outDir, isMem64 ? 'wasm_memory64' : 'wasm');
    // The .wasm file goes directly into the dist dir (also .map in debug).
    for (const ext of ['.wasm'].concat(cfg.debug ? ['.wasm.map'] : [])) {
      const src = `${wasmOutDir}/${wasmMod}${ext}`;
      addTask(cp, [src, pjoin(cfg.outDistDir, wasmMod + ext)]);
    }
    // The .js / .d.ts go into intermediates, picked up by the bundler.
    for (const ext of ['.js', '.d.ts']) {
      const fname = `${wasmMod}${ext}`;
      addTask(cp, [pjoin(wasmOutDir, fname), pjoin(cfg.outGenDir, fname)]);
    }
  }
}

export function copySyntaqliteRuntime(ctx, cfg) {
  const {ROOT_DIR, addTask, cp} = ctx;
  const srcDir = pjoin(ROOT_DIR, 'ui/node_modules/syntaqlite/wasm');
  const dstDir = pjoin(cfg.outDistRootDir, 'assets');
  for (const fname of [
    'syntaqlite-runtime.js',
    'syntaqlite-runtime.wasm',
    'syntaqlite-sqlite.wasm',
  ]) {
    addTask(cp, [pjoin(srcDir, fname), pjoin(dstDir, fname)]);
  }
  addTask(() => buildSyntaqlitePerfettoDialect(ctx, cfg), []);
}

function getBuildToolsBinDir(ROOT_DIR) {
  let binDirName;
  switch (process.platform) {
    case 'darwin':
      binDirName = 'mac';
      break;
    case 'linux':
      binDirName = 'linux64';
      break;
    default:
      throw new Error(`Unsupported platform: ${process.platform}`);
  }
  return pjoin(ROOT_DIR, 'buildtools', binDirName);
}

function buildSyntaqlitePerfettoDialect(ctx, cfg) {
  const {ROOT_DIR, exec, ensureDir} = ctx;
  const buildToolsBinDir = getBuildToolsBinDir(ROOT_DIR);
  const emcc = pjoin(buildToolsBinDir, 'emsdk/emscripten/emcc');
  const src = pjoin(
    ROOT_DIR,
    'src/trace_processor/perfetto_sql/syntaqlite/syntaqlite_perfetto.c',
  );
  const dst = pjoin(cfg.outDistRootDir, 'assets', 'syntaqlite-perfetto.wasm');
  try {
    const srcMtime = fs.statSync(src).mtimeMs;
    const dstMtime = fs.statSync(dst).mtimeMs;
    if (dstMtime >= srcMtime) return;
  } catch (e) {
    /* dst missing → rebuild */
  }
  ensureDir(path.dirname(dst));
  const emConfig = pjoin(ROOT_DIR, 'gn/standalone/.emscripten');
  const prevEmConfig = process.env.EM_CONFIG;
  process.env.EM_CONFIG = emConfig;
  try {
    exec(emcc, [
      '-O2',
      '-sSIDE_MODULE=2',
      '-sEXPORTED_FUNCTIONS=_syntaqlite_perfetto_dialect_template',
      '-o',
      dst,
      src,
    ]);
  } finally {
    if (prevEmConfig === undefined) delete process.env.EM_CONFIG;
    else process.env.EM_CONFIG = prevEmConfig;
  }
}
