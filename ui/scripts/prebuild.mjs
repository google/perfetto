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

// Prebuild: everything Vite consumes before bundling. Wasm via ninja, protos
// via pbjs/pbts, stdlib docs JSON, static asset copy, and the patched root
// index.html.

import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {buildWasm, copySyntaqliteRuntime} from './build_wasm.mjs';
import {
  copyByPattern,
  copyDir,
  ensureDir,
  listFilesRecursive,
} from './fs_utils.mjs';
import {runInProcStep, runStep} from './steps.mjs';

const pjoin = path.join;

const WASM_MODULES = [
  'traceconv',
  'proto_utils',
  'trace_processor',
  'trace_processor_memory64',
];

const PROTO_INPUTS = [
  'protos/perfetto/ipc/consumer_port.proto',
  'protos/perfetto/ipc/wire_protocol.proto',
  'protos/perfetto/trace/perfetto/perfetto_metatrace.proto',
  'protos/perfetto/perfetto_sql/structured_query.proto',
  'protos/perfetto/trace_processor/trace_processor.proto',
];

export async function prebuild({rootDir, outDir, version}) {
  await checkBuildDeps({rootDir, outDir});

  // Wipe the UI out dir for a clean prod build. Done up front so both
  // prebuild and prod write into a known-empty tree.
  await runInProcStep('clean output directory', async () => {
    await fs.promises.rm(outDir, {recursive: true, force: true});
    ensureDir(outDir);
  });

  // outDir is the UI out dir (e.g. <repo>/out/ui/ui). ninja's root out dir is
  // its parent.
  const ninjaOutDir = path.dirname(outDir);
  const distRootDir = ensureDir(pjoin(outDir, 'dist'));
  // Versioned dist subdir: dist/v<version>/. Everything except the root
  // index.html and service_worker.js lives in here so multiple versions can
  // coexist in the GCS bucket and clients can swap atomically.
  const distDir = ensureDir(pjoin(distRootDir, version));
  const genDir = ensureDir(pjoin(outDir, 'tsc/gen'));
  // Generated .js under tsc/gen (e.g. protos.js) does `require('protobufjs')`
  // etc. Node's resolver walks up from the file's directory; without this
  // symlink it never finds the real node_modules under ui/.
  const nodeModulesLink = pjoin(outDir, 'tsc/node_modules');
  if (!fs.existsSync(nodeModulesLink)) {
    fs.symlinkSync(pjoin(rootDir, 'ui/node_modules'), nodeModulesLink);
  }

  const run = (label, cmd, args) =>
    runStep(label, cmd, args, {cwd: pjoin(rootDir, 'ui')});

  await buildWasm({
    rootDir,
    ninjaOutDir,
    distDir,
    genDir,
    wasmModules: WASM_MODULES,
    run,
  });
  await copySyntaqliteRuntime({rootDir, distRootDir: distDir, run});
  await compileProtos({rootDir, genDir, run});
  await generateStdlibDocs({rootDir, distDir, run});
  await runInProcStep('copy static assets', () =>
    copyStaticAssets({
      rootDir,
      distRootDir: distDir,
      extDir: pjoin(outDir, 'chrome_extension'),
    }),
  );
  await runInProcStep('write index.html', () =>
    writeIndexHtml({rootDir, distRootDir, distDir, version}),
  );

  return {distRootDir, distDir, genDir};
}

// Checks buildtools/ matches the pinned versions in tools/install-build-deps.
// The script writes |checkDepsPath| as a stamp on success, then short-circuits
// on subsequent runs via --check-only. macOS Apple Silicon: force arm64 since
// some buildtools binaries are arm64-only. Stamp lives one level above outDir
// so the prebuild's clean step doesn't wipe it.
async function checkBuildDeps({rootDir, outDir}) {
  const checkDepsPath = pjoin(path.dirname(outDir), '.check_deps');
  let cmd = pjoin(rootDir, 'tools/install-build-deps');
  let args = [`--check-only=${checkDepsPath}`, '--ui'];
  if (process.platform === 'darwin') {
    const arm = spawnSync('arch', ['-arm64', 'true']);
    if (arm.status === 0) {
      args = ['-arch', 'arm64', cmd, ...args];
      cmd = 'arch';
    }
  }
  await runStep('install-build-deps --check-only', cmd, args);
}

export async function compileProtos({rootDir, genDir, run}) {
  const r = run ?? ((l, c, a) => runStep(l, c, a, {cwd: pjoin(rootDir, 'ui')}));
  const dstJs = pjoin(genDir, 'protos.js');
  const dstTs = pjoin(genDir, 'protos.d.ts');
  const modBin = (bin) => pjoin(rootDir, 'ui/node_modules/.bin', bin);

  // Can't put --no-comments on pbjs - the comments are load-bearing for the
  // pbts invocation which follows.
  await r('pbjs (protos.js)', modBin('pbjs'), [
    '--no-beautify',
    '--force-number',
    '--no-delimited',
    '--no-verify',
    '-t', 'static-module',
    '-w', 'es6',
    '-p', rootDir,
    '-o', dstJs,
    ...PROTO_INPUTS,
  ]);

  // Note: pbts is slow because it shells out to jsdoc to parse the comments
  // out of |dstJs|; catharsis (jsdoc's type parser) pins a CPU core throughout.
  await r('pbts (protos.d.ts)', modBin('pbts'), [
    '--no-comments',
    '-p', rootDir,
    '-o', dstTs,
    dstJs,
  ]);
}

// Generates stdlib_docs.json from the PerfettoSQL stdlib .sql files. Consumed
// at runtime by the UI's SQL docs viewer.
async function generateStdlibDocs({rootDir, distDir, run}) {
  const stdlibDir = pjoin(rootDir, 'src/trace_processor/perfetto_sql/stdlib');
  const sqlFiles = listFilesRecursive(stdlibDir).filter((f) =>
    f.endsWith('.sql'),
  );
  await run('gen_stdlib_docs_json', pjoin(rootDir, 'tools/gen_stdlib_docs_json.py'), [
    '--json-out', pjoin(distDir, 'stdlib_docs.json'),
    '--minify',
    ...sqlFiles,
  ]);
}

// Copies the static asset trees that aren't processed by Vite — PNGs and
// fonts loaded via runtime `assetSrc()` calls, data_explorer JSON/MD, and
// the chrome extension's manifest+icon.
function copyStaticAssets({rootDir, distRootDir, extDir}) {
  const assetsDst = pjoin(distRootDir, 'assets');
  copyByPattern(pjoin(rootDir, 'ui/src/assets'), assetsDst, /\.png$/);
  copyDir(
    pjoin(rootDir, 'ui/src/assets/data_explorer'),
    pjoin(assetsDst, 'data_explorer'),
    /\.(json|md)$/,
  );
  copyByPattern(pjoin(rootDir, 'buildtools/typefaces'), assetsDst, /\.woff2$/);
  copyByPattern(
    pjoin(rootDir, 'buildtools/catapult_trace_viewer'),
    assetsDst,
    /\.(js|html)$/,
  );
  ensureDir(extDir);
  fs.copyFileSync(
    pjoin(rootDir, 'ui/src/assets/logo-128.png'),
    pjoin(extDir, 'logo-128.png'),
  );
  fs.copyFileSync(
    pjoin(rootDir, 'ui/src/chrome_extension/manifest.json'),
    pjoin(extDir, 'manifest.json'),
  );
}

// Writes index.html twice:
//   dist/v<version>/index.html — verbatim copy (lets /v<version>/ serve as a
//     standalone archival entry point).
//   dist/index.html            — patched so data-perfetto_version maps the
//     'stable' channel to this build, ensuring the channel loader picks the
//     locally-built bundles instead of whatever's cached in localStorage or
//     baked into the source index.html.
function writeIndexHtml({rootDir, distRootDir, distDir, version}) {
  const src = pjoin(rootDir, 'ui/src/assets/index.html');
  const html = fs.readFileSync(src, 'utf8');
  fs.writeFileSync(pjoin(distDir, 'index.html'), html);
  const versionMap = JSON.stringify({stable: version});
  const patched = html.replace(
    /data-perfetto_version='[^']*'/,
    `data-perfetto_version='${versionMap}'`,
  );
  fs.writeFileSync(pjoin(distRootDir, 'index.html'), patched);
}
