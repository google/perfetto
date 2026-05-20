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

// Wasm build steps invoked from build.mjs. All functions are synchronous and
// exit the process on failure.
//   - buildWasm():               runs gn+ninja for trace_processor / traceconv
//                                / proto_utils Wasm modules and stages their
//                                .wasm/.js/.d.ts outputs.
//   - copySyntaqliteRuntime():   copies the prebuilt syntaqlite runtime out of
//                                node_modules and compiles our SQL dialect
//                                side module via emcc.

import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const pjoin = path.join;

function defaultRun(_label, cmd, args, opts = {}) {
  const res = spawnSync(cmd, args, {stdio: 'inherit', ...opts});
  if (res.status !== 0) {
    console.error(`${cmd} ${args.join(' ')} failed (status=${res.status})`);
    process.exit(res.status ?? 1);
  }
}

function ensureDir(p) {
  fs.mkdirSync(p, {recursive: true});
  return p;
}

function cp(src, dst) {
  ensureDir(path.dirname(dst));
  fs.copyFileSync(src, dst);
}

// |ninjaOutDir| is the root out/ dir that ninja targets (e.g. <repo>/out/ui).
// |distDir|    versioned dist (where .wasm files end up).
// |genDir|     intermediates dir (where .js/.d.ts wasm glue is staged).
// |wasmModules| list of ninja target basenames (no _wasm suffix).
// |run| (optional) is an async (label, cmd, args, opts) => void runner used
// for long-running subprocesses with their own output (gn, ninja, emcc).
// Defaults to a blocking spawnSync; build.mjs passes runStepStream for spicy
// terminal feedback.
export async function buildWasm({
  rootDir,
  ninjaOutDir,
  distDir,
  genDir,
  wasmModules,
  debug = false,
  skipBuild = false,
  noOverrideGnArgs = false,
  run = defaultRun,
}) {
  if (!skipBuild) {
    if (!noOverrideGnArgs) {
      let gnVars = `is_debug=${debug}`;
      if (spawnSync('which', ['ccache']).status === 0) {
        gnVars += ` cc_wrapper="ccache"`;
      }
      await run('gn gen (wasm)', pjoin(rootDir, 'tools/gn'), [
        'gen',
        `--args=${gnVars}`,
        ninjaOutDir,
      ]);
    }
    const ninjaArgs = ['-C', ninjaOutDir];
    ninjaArgs.push(...wasmModules.map((x) => `${x}_wasm`));
    await run('ninja (wasm)', pjoin(rootDir, 'tools/ninja'), ninjaArgs);
  }

  for (const wasmMod of wasmModules) {
    const isMem64 = wasmMod.endsWith('_memory64');
    const wasmOutDir = pjoin(ninjaOutDir, isMem64 ? 'wasm_memory64' : 'wasm');
    // The .wasm file goes directly into the dist dir (also .map in debug).
    for (const ext of ['.wasm'].concat(debug ? ['.wasm.map'] : [])) {
      cp(pjoin(wasmOutDir, `${wasmMod}${ext}`), pjoin(distDir, wasmMod + ext));
    }
    // The .js / .d.ts go into intermediates, picked up by the bundler.
    for (const ext of ['.js', '.d.ts']) {
      cp(pjoin(wasmOutDir, `${wasmMod}${ext}`), pjoin(genDir, `${wasmMod}${ext}`));
    }
  }
}

// |distRootDir| is the unversioned dist dir (where assets/ lives).
export async function copySyntaqliteRuntime({
  rootDir,
  distRootDir,
  run = defaultRun,
}) {
  const srcDir = pjoin(rootDir, 'ui/node_modules/syntaqlite/wasm');
  const dstDir = pjoin(distRootDir, 'assets');
  for (const fname of [
    'syntaqlite-runtime.js',
    'syntaqlite-runtime.wasm',
    'syntaqlite-sqlite.wasm',
  ]) {
    cp(pjoin(srcDir, fname), pjoin(dstDir, fname));
  }
  await buildSyntaqlitePerfettoDialect({rootDir, distRootDir, run});
}

function getBuildToolsBinDir(rootDir) {
  const binDirName = {darwin: 'mac', linux: 'linux64'}[process.platform];
  if (!binDirName) {
    throw new Error(`Unsupported platform: ${process.platform}`);
  }
  return pjoin(rootDir, 'buildtools', binDirName);
}

async function buildSyntaqlitePerfettoDialect({rootDir, distRootDir, run}) {
  const emcc = pjoin(getBuildToolsBinDir(rootDir), 'emsdk/emscripten/emcc');
  const src = pjoin(
    rootDir,
    'src/trace_processor/perfetto_sql/syntaqlite/syntaqlite_perfetto.c',
  );
  const dst = pjoin(distRootDir, 'assets', 'syntaqlite-perfetto.wasm');
  try {
    if (fs.statSync(dst).mtimeMs >= fs.statSync(src).mtimeMs) return;
  } catch (e) {
    /* dst missing → rebuild */
  }
  ensureDir(path.dirname(dst));
  const prevEmConfig = process.env.EM_CONFIG;
  process.env.EM_CONFIG = pjoin(rootDir, 'gn/standalone/.emscripten');
  try {
    await run('emcc (syntaqlite dialect)', emcc, [
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
