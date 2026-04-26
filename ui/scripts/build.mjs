#!/usr/bin/env node
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

// Orchestrator for the Vite-based UI build. Replaces ui/build.js.
//
// CLI surface is the subset of ui/build.js's flags that external callers
// (ui/build, ui/run-dev-server, CI) rely on. --serve and --watch are handled
// by scripts/dev.mjs.

import fs from 'node:fs';
import {join, relative} from 'node:path';
import {
  UI_DIR,
  ROOT_DIR,
  exec,
  ensureDir,
  ensureSymlinks,
  loadDevServerEnvFile,
  makeLayout,
  readVersion,
  resolveOutDir,
} from './common.mjs';
import {runAll as runCodegen} from './codegen.mjs';
import {buildWasm, stageWasm} from './build_wasm.mjs';
import {
  copyChromeExtensionAssets,
  copyHtml,
  copyStaticAssets,
} from './assets.mjs';
import {
  generateServiceWorkerManifest,
  writeRootIndexHtml,
} from './postbuild.mjs';
import {allBundles, configForBundle} from '../vite/config.mjs';

// Precedence (lowest to highest):
//   1. Built-in defaults (the initial `flags` object below)
//   2. ~/.config/perfetto/ui-dev-server.env — loaded into process.env
//   3. PERFETTO_UI_* environment variables
//   4. CLI flags
function parseArgs(argv) {
  const flags = {
    out: null,
    minifyJs: null, // 'preserve_comments' | 'all' | null
    noSourceMaps: false,
    noTreeshake: false, // kept for CLI compat; doesn't affect Vite today
    verbose: false,
    noBuild: false,
    noWasm: false,
    onlyWasmMemory64: false,
    debug: false,
    bigtrace: false,
    openPerfettoTrace: false,
    noDepsCheck: false,
    typecheck: false,
    noOverrideGnArgs: false,
    title: '',
  };

  // (2) + (3): pull defaults from the .env file and PERFETTO_UI_* vars.
  loadDevServerEnvFile();
  for (const [k, v] of Object.entries(process.env)) {
    if (!k.startsWith('PERFETTO_UI_')) continue;
    const name = k.slice('PERFETTO_UI_'.length).toLowerCase();
    switch (name) {
      case 'no_build': case 'n': flags.noBuild = coerceBool(v); break;
      case 'no_wasm': case 'w': flags.noWasm = coerceBool(v); break;
      case 'only_wasm_memory64': flags.onlyWasmMemory64 = coerceBool(v); break;
      case 'debug': case 'd': flags.debug = coerceBool(v); break;
      case 'verbose': case 'v': flags.verbose = coerceBool(v); break;
      case 'minify_js': flags.minifyJs = v; break;
      case 'title': flags.title = v; break;
      case 'bigtrace': flags.bigtrace = coerceBool(v); break;
      case 'open_perfetto_trace': flags.openPerfettoTrace = coerceBool(v); break;
      case 'typecheck': flags.typecheck = coerceBool(v); break;
      case 'no_source_maps': flags.noSourceMaps = coerceBool(v); break;
      case 'no_depscheck': flags.noDepsCheck = coerceBool(v); break;
      case 'no_override_gn_args': flags.noOverrideGnArgs = coerceBool(v); break;
      case 'out': flags.out = v; break;
    }
  }

  // (4): CLI flags — these always win.
  const rest = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    switch (a) {
      case '--out': flags.out = argv[++i]; break;
      case '--minify-js':
      case '--minify-js=all':
      case '--minify-js=preserve_comments': {
        if (a.includes('=')) flags.minifyJs = a.split('=')[1];
        else flags.minifyJs = argv[++i];
        break;
      }
      case '--no-source-maps': flags.noSourceMaps = true; break;
      case '--no-treeshake': flags.noTreeshake = true; break;
      case '--verbose': case '-v': flags.verbose = true; break;
      case '--no-build': case '-n': flags.noBuild = true; break;
      case '--no-wasm': case '-W': flags.noWasm = true; break;
      case '--only-wasm-memory64': flags.onlyWasmMemory64 = true; break;
      case '--debug': case '-d': flags.debug = true; break;
      case '--bigtrace': flags.bigtrace = true; break;
      case '--open-perfetto-trace': flags.openPerfettoTrace = true; break;
      case '--no-depscheck': flags.noDepsCheck = true; break;
      case '--typecheck': flags.typecheck = true; break;
      case '--no-override-gn-args': flags.noOverrideGnArgs = true; break;
      case '--title': flags.title = argv[++i]; break;
      // forwarded to scripts/dev.mjs, ignored here
      case '--serve': case '-s':
      case '--serve-host':
      case '--serve-port':
      case '--watch': case '-w':
      case '--interactive': case '-i':
      case '--rebaseline': case '-r':
      case '--cross-origin-isolation':
      case '--test-filter': case '-f':
      case '--run-unittests': case '-t':
        // consume value-bearing flags' values too
        if (['--serve-host', '--serve-port', '--test-filter', '-f'].includes(a)) {
          i++;
        }
        break;
      default:
        if (a.startsWith('--')) {
          console.warn(`Ignoring unknown flag ${a}`);
        } else {
          rest.push(a);
        }
    }
  }
  return flags;
}

function coerceBool(v) {
  return v === '1' || v === 'true' || v === 'TRUE';
}

// pid-based lock to prevent concurrent builds stomping on the output dir.
// Ported from ui/build.js::prepareBuildLock.
function acquireBuildLock(outDir) {
  const lockFile = join(outDir, 'watch.lock');
  if (fs.existsSync(lockFile)) {
    const oldPid = fs.readFileSync(lockFile, 'utf8').trim();
    let running = true;
    try {
      process.kill(parseInt(oldPid, 10), 0);
    } catch {
      running = false;
    }
    if (running) {
      console.error(
        `Error: a build instance is already running (PID=${oldPid}, lock=${lockFile}).`,
      );
      console.error('Hint: use --no-build (-n) to skip the build and avoid the lock.');
      process.exit(1);
    }
    console.log(`Removing stale lock file for PID ${oldPid}`);
    fs.unlinkSync(lockFile);
  }
  fs.writeFileSync(lockFile, String(process.pid));
  const release = () => {
    try {
      if (fs.existsSync(lockFile)) {
        const pid = fs.readFileSync(lockFile, 'utf8').trim();
        if (pid === String(process.pid)) fs.unlinkSync(lockFile);
      }
    } catch {
      // ignore
    }
  };
  process.on('exit', release);
  for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP']) {
    process.once(sig, () => {
      release();
      process.kill(process.pid, sig);
    });
  }
}

async function runViteBuild(bundle, layout, opts) {
  const vite = await import('vite');
  const cfg = configForBundle(bundle, layout, opts);
  const t0 = performance.now();
  await vite.build(cfg);
  const ms = (performance.now() - t0).toFixed(0);
  console.log(`[vite] built ${bundle.name} in ${ms}ms`);
}

async function maybeCheckDeps() {
  const checkDepsPath = join(ROOT_DIR, 'out/ui/.check_deps');
  const installBuildDeps = join(ROOT_DIR, 'tools/install-build-deps');
  let args = [installBuildDeps, `--check-only=${checkDepsPath}`, '--ui'];
  if (process.platform === 'darwin') {
    try {
      const {execFileSync} = await import('node:child_process');
      execFileSync('arch', ['-arm64', 'true']);
      args = ['arch', '-arch', 'arm64', ...args];
    } catch {
      // Not arm64-capable, fall through.
    }
  }
  const cmd = args.shift();
  exec(cmd, args);
}

function compileScss(layout, {watch = false} = {}) {
  const src = join(UI_DIR, 'src/assets/perfetto.scss');
  const dst = join(layout.outDistDir, 'perfetto.css');
  ensureDir(layout.outDistDir);
  const args = [src, dst];
  // Quiet unless verbose to match the old build.
  args.unshift('--quiet');
  if (watch) args.unshift('--watch');
  exec(join(UI_DIR, 'node_modules/.bin/sass'), args);
}

function tscNoEmit(project) {
  exec(join(UI_DIR, 'node_modules/.bin/tsc'), [
    '--project',
    join(ROOT_DIR, project),
    '--noEmit',
  ]);
}

export async function runBuild(flags) {
  const outDir = resolveOutDir(flags.out);
  ensureDir(outDir);
  const version = readVersion();
  const layout = makeLayout(outDir, version);
  // Match the old ui/build.js behavior: on a full (non --no-build) build we
  // wipe `<outDir>/ui/` to ensure stale artifacts from previous builds (e.g.
  // wasm modules that are no longer part of the build, or deleted plugins)
  // do not linger and end up in manifest.json.
  if (!flags.noBuild) {
    ensureDir(layout.outUiDir, {clean: true});
  }
  ensureDir(layout.outUiDir);
  ensureDir(layout.outTscDir);
  ensureDir(layout.outDistRootDir);
  ensureDir(layout.outDistDir);
  if (flags.bigtrace) ensureDir(layout.outBigtraceDistDir);
  if (flags.openPerfettoTrace) ensureDir(layout.outOpenPerfettoTraceDistDir);
  ensureDir(layout.outExtDir);
  ensureSymlinks(layout);

  if (!flags.noBuild) acquireBuildLock(outDir);
  if (!flags.noDepsCheck) await maybeCheckDeps();

  // 1) Codegen: protos, version, plugin indexes, stdlib docs.
  runCodegen(layout);

  // 2) Wasm: build + stage (skipped under --no-wasm).
  if (flags.noWasm) {
    // Always stage whatever is already on disk so the Vite resolvers find
    // the glue .js/.d.ts + .wasm alongside the other dist outputs.
    stageWasm(layout, {
      onlyMemory64: flags.onlyWasmMemory64,
      debug: flags.debug,
    });
  } else {
    buildWasm(layout, {
      onlyMemory64: flags.onlyWasmMemory64,
      debug: flags.debug,
      noOverrideGnArgs: flags.noOverrideGnArgs,
    });
  }

  // 3) Static assets + HTML into dist/<ver>/.
  copyStaticAssets({
    outDistDir: layout.outDistDir,
    outBigtraceDistDir: flags.bigtrace ? layout.outBigtraceDistDir : null,
  });
  copyHtml({
    outDistDir: layout.outDistDir,
    outOpenPerfettoTraceDistDir: layout.outOpenPerfettoTraceDistDir,
    bigtrace: flags.bigtrace,
    openPerfettoTrace: flags.openPerfettoTrace,
  });
  copyChromeExtensionAssets({outExtDir: layout.outExtDir});

  if (flags.typecheck) {
    const projects = ['ui', 'ui/src/service_worker'];
    if (flags.bigtrace) projects.push('ui/src/bigtrace');
    if (flags.openPerfettoTrace) projects.push('ui/src/open_perfetto_trace');
    for (const p of projects) tscNoEmit(p);
    return;
  }

  if (flags.noBuild) return;

  // 4) SCSS.
  compileScss(layout);
  if (flags.bigtrace) {
    // Old build also copies perfetto.css into the bigtrace subdir.
    const {cp} = await import('./common.mjs');
    cp(
      join(layout.outDistDir, 'perfetto.css'),
      join(layout.outBigtraceDistDir, 'perfetto.css'),
    );
  }

  // 5) Vite: one build per bundle, sequentially.
  const opts = {
    minify: flags.minifyJs === 'all' || flags.minifyJs === 'preserve_comments'
      ? 'terser'
      : false,
    sourceMaps: !flags.noSourceMaps,
    mode: 'production',
    verbose: flags.verbose,
  };
  const bundles = allBundles(layout, {
    bigtrace: flags.bigtrace,
    openPerfettoTrace: flags.openPerfettoTrace,
  });
  for (const bundle of bundles) {
    await runViteBuild(bundle, layout, opts);
  }

  // 6) Post-build: manifest.json, dist/index.html.
  generateServiceWorkerManifest(layout.outDistDir);
  writeRootIndexHtml({
    outDistDir: layout.outDistDir,
    outDistRootDir: layout.outDistRootDir,
    version,
    titleOverride: flags.title,
  });
  if (flags.bigtrace) {
    writeRootIndexHtml({
      outDistDir: layout.outDistDir,
      outDistRootDir: layout.outDistRootDir,
      version,
      titleOverride: flags.title,
      indexFile: 'bigtrace.html',
    });
  }
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const flags = parseArgs(process.argv.slice(2));
  runBuild(flags).catch((err) => {
    console.error(err);
    process.exit(1);
  });
}
