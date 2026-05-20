// Copyright (C) 2021 The Android Open Source Project
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

// This script takes care of:
// - The build process for the whole UI and the chrome extension.
// - The HTTP dev-server with live-reload capabilities.
// The reason why this is a hand-rolled script rather than a conventional build
// system is keeping incremental build fast and maintaining the set of
// dependencies contained.
// The only way to keep incremental build fast (i.e. O(seconds) for the
// edit-one-line -> reload html cycles) is to run both the TypeScript compiler
// and the rollup bundler in --watch mode. Any other attempt, leads to O(10s)
// incremental-build times.
// This script allows mixing build tools that support --watch mode (tsc and
// rollup) and auto-triggering-on-file-change rules via fs.watch.
// When invoked without any argument (e.g., for production builds), this script
// just runs all the build tasks serially. It doesn't to do any mtime-based
// check, it always re-runs all the tasks.
// When invoked with --watch, it mounts a pipeline of tasks based on fs.watch
// and runs them together with tsc --watch and rollup --watch.
// The output directory structure is carefully crafted so that any change to UI
// sources causes cascading triggers of the next steps.
// The overall build graph looks as follows:
// +----------------+      +-----------------------------+
// | protos/*.proto |----->| pbjs out/tsc/gen/protos.js  |--+
// +----------------+      +-----------------------------+  |
//                         +-----------------------------+  |
//                         | pbts out/tsc/gen/protos.d.ts|<-+
//                         +-----------------------------+
//                             |
//                             V      +-------------------------+
// +---------+              +-----+   |  out/tsc/frontend/*.js  |
// | ui/*.ts |------------->| tsc |-> +-------------------------+   +--------+
// +---------+              +-----+   | out/tsc/controller/*.js |-->| rollup |
//                            ^       +-------------------------+   +--------+
//                +------------+      |   out/tsc/engine/*.js   |       |
// +-----------+  |*.wasm.js   |      +-------------------------+       |
// |ninja *.cc |->|*.wasm.d.ts |                                        |
// +-----------+  |*.wasm      |-----------------+                      |
//                +------------+                 |                      |
//                                               V                      V
// +-----------+  +------+    +------------------------------------------------+
// | ui/*.scss |->| scss |--->|              Final out/dist/ dir               |
// +-----------+  +------+    +------------------------------------------------+
// +----------------------+   | +----------+ +---------+ +--------------------+|
// | src/assets/*.png     |   | | assets/  | |*.wasm.js| | frontend_bundle.js ||
// +----------------------+   | |  *.css   | |*.wasm   | +--------------------+|
// | buildtools/typefaces |-->| |  *.png   | +---------+ |  engine_bundle.js  ||
// +----------------------+   | |  *.woff2 |             +--------------------+|
// | buildtools/legacy_tv |   | |  tv.html |             |traceconv_bundle.js ||
// +----------------------+   | +----------+             +--------------------+|
//                            +------------------------------------------------+

import argparse from 'argparse';
import childProcess from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {startStaticServer} from './static_server.mjs';
import {buildWasm, copySyntaqliteRuntime} from './build_wasm.mjs';

const pjoin = path.join;
const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const ROOT_DIR = path.dirname(__dirname); // The repo root.
const VERSION_SCRIPT = pjoin(ROOT_DIR, 'tools/write_version_header.py');
const DEFAULT_PORT = 10000;

const cfg = {
  minifyJs: '',
  noSourceMaps: false,
  noTreeshake: false,
  watch: false,
  verbose: false,
  debug: false,
  bigtrace: false,
  startHttpServer: false,
  httpServerListenHost: '127.0.0.1',
  httpServerListenPort: undefined,
  onlyWasmMemory64: false,
  wasmModules: [],
  crossOriginIsolation: false,
  testFilter: '',
  noOverrideGnArgs: false,

  // The fields below will be changed by main() after cmdline parsing.
  // Directory structure:
  // out/xxx/    -> outDir         : Root build dir, for both ninja/wasm and UI.
  //   ui/       -> outUiDir       : UI dir. All outputs from this script.
  //    tsc/     -> outTscDir      : Transpiled .ts -> .js.
  //      gen/   -> outGenDir      : Auto-generated .ts/.js (e.g. protos).
  //    dist/    -> outDistRootDir : Only index.html and service_worker.js
  //      v1.2/  -> outDistDir     : JS bundles and assets
  //    chrome_extension/          : Chrome extension.
  outDir: pjoin(ROOT_DIR, 'out/ui'),
  version: '', // v1.2.3, derived from the CHANGELOG + git.
  outUiDir: '',
  outUiTestArtifactsDir: '',
  outDistRootDir: '',
  outTscDir: '',
  outGenDir: '',
  outDistDir: '',
  outExtDir: '',
  outBigtraceDistDir: '',
  outOpenPerfettoTraceDistDir: '',
  lockFile: '',
};

const RULES = [
  {r: /ui\/src\/assets\/index.html/, f: copyIndexHtml},
  {r: /ui\/src\/assets\/bigtrace.html/, f: copyBigtraceHtml},
  {r: /ui\/src\/open_perfetto_trace\/index.html/, f: copyOpenPerfettoTraceHtml},
  {r: /ui\/src\/assets\/((.*)[.]png)/, f: copyAssets},
  {r: /ui\/src\/assets\/(data_explorer\/base-page\.json)/, f: copyAssets},
  {r: /ui\/src\/assets\/(data_explorer\/examples\/(.*)[.]json)/, f: copyAssets},
  {r: /ui\/src\/assets\/(data_explorer\/node_info\/(.*)[.]md)/, f: copyAssets},
  {r: /buildtools\/typefaces\/(.+[.]woff2)/, f: copyAssets},
  {r: /buildtools\/catapult_trace_viewer\/(.+(js|html))/, f: copyAssets},
  {r: /ui\/src\/chrome_extension\/.*/, f: copyExtensionAssets},
  {r: /.*\/dist\/.+\/(?!manifest\.json).*/, f: genServiceWorkerManifestJson},
];

const tasks = [];
let tasksTot = 0;
let tasksRan = 0;
const tStart = performance.now();
const subprocesses = [];

// Loads ~/.config/perfetto/ui-dev-server.env and injects any KEY=VALUE pairs
// into process.env, without overriding variables already set in the environment.
function loadDevServerEnvFile() {
  const home = process.env.HOME || process.env.USERPROFILE || '';
  const envFile = path.join(home, '.config', 'perfetto', 'ui-dev-server.env');
  let content;
  try {
    content = fs.readFileSync(envFile, 'utf8');
  } catch (e) {
    return; // File absent or unreadable — not an error.
  }
  for (const line of content.split('\n')) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith('#')) continue;
    const eqIdx = trimmed.indexOf('=');
    if (eqIdx < 0) continue;
    const key = trimmed.slice(0, eqIdx).trim();
    const value = trimmed.slice(eqIdx + 1).trim();
    if (!(key in process.env)) process.env[key] = value;
  }
}

async function main() {
  const parser = new argparse.ArgumentParser({
    formatter_class: argparse.RawDescriptionHelpFormatter,
    epilog: `
Env-var overrides:
  Any flag can be set via a PERFETTO_UI_<FLAG> environment variable,
  where <FLAG> is the flag name uppercased with hyphens replaced by
  underscores. Boolean flags are activated by "1" or "true". CLI flags
  always take precedence over environment variables.

  Examples:
    PERFETTO_UI_SERVE_HOST=0.0.0.0
    PERFETTO_UI_SERVE_PORT=10000
    PERFETTO_UI_NO_BUILD=1
    PERFETTO_UI_TITLE=my-instance

  Defaults can also be persisted in:
    ~/.config/perfetto/ui-dev-server.env
  (one KEY=VALUE per line, # comments supported). Shell env vars take
  precedence over the file.
`,
  });
  parser.add_argument('--out', {help: 'Output directory'});
  parser.add_argument('--minify-js', {
    help: 'Minify js files',
    choices: ['preserve_comments', 'all'],
  });
  parser.add_argument('--no-source-maps', {
    action: 'store_true',
    help: 'Skip source map generation entirely',
  });
  parser.add_argument('--no-treeshake', {
    action: 'store_true',
    help: 'Disable rollup tree-shaking (much faster incremental rebuilds)',
  });
  parser.add_argument('--watch', '-w', {action: 'store_true'});
  parser.add_argument('--serve', '-s', {action: 'store_true'});
  parser.add_argument('--serve-host', {help: '--serve bind host'});
  parser.add_argument('--serve-port', {help: '--serve bind port', type: 'int'});
  parser.add_argument('--verbose', '-v', {action: 'store_true'});
  parser.add_argument('--no-build', '-n', {action: 'store_true'});
  parser.add_argument('--no-wasm', '-W', {action: 'store_true'});
  parser.add_argument('--only-wasm-memory64', {action: 'store_true'});
  parser.add_argument('--run-unittests', '-t', {action: 'store_true'});
  parser.add_argument('--debug', '-d', {action: 'store_true'});
  parser.add_argument('--bigtrace', {action: 'store_true'});
  parser.add_argument('--open-perfetto-trace', {action: 'store_true'});
  parser.add_argument('--interactive', '-i', {action: 'store_true'});
  parser.add_argument('--rebaseline', '-r', {action: 'store_true'});
  parser.add_argument('--no-depscheck', {action: 'store_true'});
  parser.add_argument('--cross-origin-isolation', {action: 'store_true'});
  parser.add_argument('--test-filter', '-f', {
    help: "filter Jest tests by regex, e.g. 'chrome_render'",
  });
  parser.add_argument('--no-override-gn-args', {action: 'store_true'});
  parser.add_argument('--typecheck', {
    action: 'store_true',
    help: 'Only type-check (tsc --noEmit), skip bundling',
  });
  parser.add_argument('--title', {
    help: 'Override the page title (useful for distinguishing multiple instances)',
  });

  // New commands for dev/preview server modes
  parser.add_argument('--preview', {
    action: 'store_true',
    help: 'Preview the build output in a temporary HTTP server',
  });
  parser.add_argument('--dev', {
    action: 'store_true',
    help: 'Start a dev server with watch mode and live reload',
  });

  // Load ~/.config/perfetto/ui-dev-server.env defaults, then map any
  // PERFETTO_UI_* env vars to synthetic argv entries prepended before the
  // real argv so that explicit CLI flags always take precedence.
  loadDevServerEnvFile();
  const envPrefix = 'PERFETTO_UI_';
  const syntheticArgv = [];
  for (const [key, val] of Object.entries(process.env)) {
    if (!key.startsWith(envPrefix)) continue;
    const flag =
      '--' + key.slice(envPrefix.length).toLowerCase().replace(/_/g, '-');
    const action = parser._actions.find((a) =>
      (a.option_strings || []).includes(flag),
    );
    if (!action) continue;
    const isBoolFlag = action.nargs === 0;
    if (isBoolFlag) {
      if (val === '1' || val.toLowerCase() === 'true') syntheticArgv.push(flag);
    } else {
      syntheticArgv.push(`${flag}=${val}`);
    }
  }
  const args = parser.parse_args([...syntheticArgv, ...process.argv.slice(2)]);
  const clean = !args.no_build;
  cfg.outDir = path.resolve(ensureDir(args.out || cfg.outDir));
  cfg.lockFile = pjoin(cfg.outDir, 'watch.lock');

  // Only create the build lock if we are actually going to build If --no-build
  // is passed, we can run simultaneoushy without worrying about the build lock,
  // since we won't be writing to the output directories.
  if (!args.no_build) {
    prepareBuildLock();
  }

  cfg.outUiDir = ensureDir(pjoin(cfg.outDir, 'ui'), clean);
  cfg.outUiTestArtifactsDir = ensureDir(pjoin(cfg.outDir, 'ui-test-artifacts'));
  cfg.outExtDir = ensureDir(pjoin(cfg.outUiDir, 'chrome_extension'));
  cfg.outDistRootDir = ensureDir(pjoin(cfg.outUiDir, 'dist'));
  const proc = exec('python3', [VERSION_SCRIPT, '--stdout'], {stdout: 'pipe'});
  cfg.version = proc.stdout.toString().trim();
  cfg.outDistDir = ensureDir(pjoin(cfg.outDistRootDir, cfg.version));
  cfg.outTscDir = ensureDir(pjoin(cfg.outUiDir, 'tsc'));
  cfg.outGenDir = ensureDir(pjoin(cfg.outUiDir, 'tsc/gen'));
  cfg.testFilter = args.test_filter || '';
  cfg.watch = !!args.watch;
  cfg.verbose = !!args.verbose;
  cfg.debug = !!args.debug;
  cfg.bigtrace = !!args.bigtrace;
  cfg.openPerfettoTrace = !!args.open_perfetto_trace;
  cfg.startHttpServer = args.serve;
  cfg.noOverrideGnArgs = !!args.no_override_gn_args;
  if (args.minify_js) {
    cfg.minifyJs = args.minify_js;
  }
  cfg.noSourceMaps = !!args.no_source_maps;
  cfg.noTreeshake = !!args.no_treeshake;
  if (args.bigtrace) {
    cfg.outBigtraceDistDir = ensureDir(pjoin(cfg.outDistDir, 'bigtrace'));
  }
  if (cfg.openPerfettoTrace) {
    cfg.outOpenPerfettoTraceDistDir = ensureDir(
      pjoin(cfg.outDistRootDir, 'open_perfetto_trace'),
    );
  }
  if (args.serve_host) {
    cfg.httpServerListenHost = args.serve_host;
  }
  if (args.serve_port) {
    cfg.httpServerListenPort = args.serve_port;
  }
  if (args.interactive) {
    process.env.PERFETTO_UI_TESTS_INTERACTIVE = '1';
  }
  if (args.rebaseline) {
    process.env.PERFETTO_UI_TESTS_REBASELINE = '1';
  }
  if (args.cross_origin_isolation) {
    cfg.crossOriginIsolation = true;
  }
  cfg.check = !!args.typecheck;
  cfg.onlyWasmMemory64 = !!args.only_wasm_memory64;
  cfg.titleOverride = args.title || '';
  cfg.wasmModules = ['traceconv', 'proto_utils', 'trace_processor_memory64'];
  if (!cfg.onlyWasmMemory64) {
    cfg.wasmModules.push('trace_processor');
  }

  function terminateChildProcesses() {
    for (const proc of subprocesses) {
      console.log(`Terminating child process with PID ${proc.pid}`);
      proc.kill(); // SIGTERM is the default
    }
  }

  // Called whenever the process exits due to:
  // 1. The JS loop running out of work - (normal exit or uncaught exception).
  // 2. Manually calling process.exit().
  process.on('exit', () => {
    terminateChildProcesses();
  });

  // Register signal handlers for the usual termination signals. Only handle
  // once per signal so we can then call process.kill(sig) in order for the
  // process to exit with the correct exit code.
  for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP']) {
    process.once(sig, () => {
      terminateChildProcesses();
      process.kill(process.pid, sig);
    });
  }

  if (!args.no_depscheck) {
    // Check that deps are current before starting.
    const installBuildDeps = pjoin(ROOT_DIR, 'tools/install-build-deps');
    const checkDepsPath = pjoin(cfg.outDir, '.check_deps');
    let args = [installBuildDeps, `--check-only=${checkDepsPath}`, '--ui'];

    if (process.platform === 'darwin') {
      const result = childProcess.spawnSync('arch', ['-arm64', 'true']);
      const isArm64Capable = result.status === 0;
      if (isArm64Capable) {
        const archArgs = ['arch', '-arch', 'arm64'];
        args = archArgs.concat(args);
      }
    }
    const cmd = args.shift();
    exec(cmd, args);
  }

  console.log('Entering', cfg.outDir);
  process.chdir(cfg.outDir);

  // Enqueue empty task. This is needed only for --no-build --serve. The HTTP
  // server is started when the task queue reaches quiescence, but it takes at
  // least one task for that.
  addTask(() => {});

  if (args.no_build && cfg.check) {
    // --no-build --typecheck: just run tsc --noEmit assuming out dir exists.
    const tsProjects = ['ui', 'ui/src/service_worker'];
    if (cfg.bigtrace) tsProjects.push('ui/src/bigtrace');
    if (cfg.openPerfettoTrace) tsProjects.push('ui/src/open_perfetto_trace');
    for (const prj of tsProjects) {
      transpileTsProject(prj, {noEmit: true});
    }
  } else if (!args.no_build) {
    updateSymlinks(); // Links //ui/out -> //out/xxx/ui/

    const wasmCtx = {ROOT_DIR, addTask, exec, cp, ensureDir};
    buildWasm(wasmCtx, cfg, args.no_wasm);
    copySyntaqliteRuntime(wasmCtx, cfg);
    scanDir('ui/src/assets');
    scanDir('ui/src/chrome_extension');
    scanDir('buildtools/typefaces');
    scanDir('buildtools/catapult_trace_viewer');
    compileProtos();
    generateStdlibDocs();

    const tsProjects = ['ui', 'ui/src/service_worker'];
    if (cfg.bigtrace) tsProjects.push('ui/src/bigtrace');
    if (cfg.openPerfettoTrace) {
      scanDir('ui/src/open_perfetto_trace');
      tsProjects.push('ui/src/open_perfetto_trace');
    }

    if (cfg.check) {
      for (const prj of tsProjects) {
        transpileTsProject(prj, {noEmit: true});
      }
    } else {
      // Vite owns TS transpile + bundling; type checking runs in-process via
      // vite-plugin-checker (see ui/vite.config.mjs).
      runVite();
      genServiceWorkerManifestJson();

      // Watches /dist to regenerate the ServiceWorker file map on change.
      scanDir(cfg.outDistRootDir);
    }
  }

  // We should enter the loop only in watch mode, where tsc and rollup are
  // asynchronous because they run in watch mode.
  if (args.no_build && !isDistComplete()) {
    console.log('No build was requested, but artifacts are not available.');
    console.log('In case of execution error, re-run without --no-build.');
  }
  if (!args.no_build && !cfg.check) {
    const tStart = performance.now();
    while (!isDistComplete()) {
      const secs = Math.ceil((performance.now() - tStart) / 1000);
      process.stdout.write(
        `\t\tWaiting for first build to complete... ${secs} s\r`,
      );
      await new Promise((r) => setTimeout(r, 500));
    }
  }
  if (cfg.watch) console.log('\nFirst build completed!');

  if (cfg.startHttpServer) {
    if (cfg.watch) {
      await startViteDevServer();
    } else {
      startStaticServer({
        rootDir: ROOT_DIR,
        distRootDir: cfg.outDistRootDir,
        host: cfg.httpServerListenHost,
        port: cfg.httpServerListenPort,
        defaultPort: DEFAULT_PORT,
        portWasExplicit: cfg.httpServerListenPort !== undefined,
        crossOriginIsolation: cfg.crossOriginIsolation,
      });
    }
  }
  if (args.run_unittests) {
    runTests();
  }
}

// -----------
// Build rules
// -----------

function runTests() {
  // Vitest reads ui/vitest.config.mjs by default. ts is transpiled on the fly
  // by Vite, so there's no tsc-emitted .js layer involved.
  const args = [
    cfg.watch ? 'watch' : 'run',
    '--config',
    pjoin(ROOT_DIR, 'ui/vitest.config.mjs'),
  ];
  if (cfg.testFilter.length > 0) {
    args.push('-t', cfg.testFilter);
  }
  if (cfg.watch) {
    addTask(execModule, ['vitest', args, {async: true}]);
  } else {
    addTask(execModule, ['vitest', args]);
  }
}

function cpHtml(src, filename) {
  let html = fs.readFileSync(src).toString();
  // First copy the html as-is into the dist/v1.2.3/ directory. This is
  // only used for archival purporses, so one can open
  // ui.perfetto.dev/v1.2.3/ to skip the auto-update and channel logic.
  fs.writeFileSync(pjoin(cfg.outDistDir, filename), html);

  // Then copy it into the dist/ root by patching the version code.
  // TODO(primiano): in next CLs, this script should take a
  // --release_map=xxx.json argument, to populate this with multiple channels.
  const versionMap = JSON.stringify({stable: cfg.version});
  const bodyRegex = /data-perfetto_version='[^']*'/;
  html = html.replace(bodyRegex, `data-perfetto_version='${versionMap}'`);

  // If --title was provided, patch the page title. Useful when running
  // multiple dev server instances to distinguish browser tabs.
  if (cfg.titleOverride) {
    html = html.replace(
      /<title>[^<]*<\/title>/,
      `<title>${cfg.titleOverride}</title>`,
    );
  }

  fs.writeFileSync(pjoin(cfg.outDistRootDir, filename), html);
}

function copyIndexHtml(src) {
  addTask(cpHtml, [src, 'index.html']);
}

function copyBigtraceHtml(src) {
  if (cfg.bigtrace) {
    addTask(cpHtml, [src, 'bigtrace.html']);
  }
}

function copyOpenPerfettoTraceHtml(src) {
  if (cfg.openPerfettoTrace) {
    addTask(cp, [src, pjoin(cfg.outOpenPerfettoTraceDistDir, 'index.html')]);
  }
}

function copyAssets(src, dst) {
  addTask(cp, [src, pjoin(cfg.outDistDir, 'assets', dst)]);
  if (cfg.bigtrace) {
    addTask(cp, [src, pjoin(cfg.outBigtraceDistDir, 'assets', dst)]);
  }
}

function copyUiTestArtifactsAssets(src, dst) {
  addTask(cp, [src, pjoin(cfg.outUiTestArtifactsDir, dst)]);
}

function compileProtos() {
  const dstJs = pjoin(cfg.outGenDir, 'protos.js');
  const dstTs = pjoin(cfg.outGenDir, 'protos.d.ts');
  const inputs = [
    'protos/perfetto/ipc/consumer_port.proto',
    'protos/perfetto/ipc/wire_protocol.proto',
    'protos/perfetto/trace/perfetto/perfetto_metatrace.proto',
    'protos/perfetto/perfetto_sql/structured_query.proto',
    'protos/perfetto/trace_processor/trace_processor.proto',
  ];
  // Can't put --no-comments here - The comments are load bearing for
  // the pbts invocation which follows.
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
  ].concat(inputs);
  addTask(execModule, ['pbjs', pbjsArgs]);

  // Note: If you are looking into slowness of pbts it is not pbts
  // itself that is slow. It invokes jsdoc to parse the comments out of
  // the |dstJs| with https://github.com/hegemonic/catharsis which is
  // pinning a CPU core the whole time.
  const pbtsArgs = ['--no-comments', '-p', ROOT_DIR, '-o', dstTs, dstJs];
  addTask(execModule, ['pbts', pbtsArgs]);
}

function generateStdlibDocs() {
  const cmd = pjoin(ROOT_DIR, 'tools/gen_stdlib_docs_json.py');
  const stdlibDir = pjoin(ROOT_DIR, 'src/trace_processor/perfetto_sql/stdlib');

  const stdlibFiles = listFilesRecursive(stdlibDir).filter(
    (filePath) => path.extname(filePath) === '.sql',
  );

  addTask(exec, [
    cmd,
    [
      '--json-out',
      pjoin(cfg.outDistDir, 'stdlib_docs.json'),
      '--minify',
      ...stdlibFiles,
    ],
  ]);
}

function updateSymlinks() {
  // /ui/out -> /out/ui.
  mklink(cfg.outUiDir, pjoin(ROOT_DIR, 'ui/out'));

  // /ui/src/gen -> /out/ui/ui/tsc/gen)
  mklink(cfg.outGenDir, pjoin(ROOT_DIR, 'ui/src/gen'));

  // /out/ui/test/data -> /test/data (For UI tests).
  mklink(
    pjoin(ROOT_DIR, 'test/data'),
    pjoin(ensureDir(pjoin(cfg.outDir, 'test')), 'data'),
  );

  // Creates a out/dist_version -> out/dist/v1.2.3 symlink, so rollup config
  // can point to that without having to know the current version number.
  mklink(
    path.relative(cfg.outUiDir, cfg.outDistDir),
    pjoin(cfg.outUiDir, 'dist_version'),
  );

  mklink(
    pjoin(ROOT_DIR, 'ui/node_modules'),
    pjoin(cfg.outTscDir, 'node_modules'),
  );
}

// This transpiles all the sources (frontend, controller, engine, extension) in
// one go. The only project that has a dedicated invocation is service_worker.
function transpileTsProject(project, options) {
  const args = ['--project', pjoin(ROOT_DIR, project)];
  options = options || {};

  if (options.noEmit) args.push('--noEmit');

  if (options.watch) {
    args.push('--watch', '--preserveWatchOutput');
    addTask(execModule, [
      'tsc',
      args,
      {
        async: true,
        noErrCheck: options.noErrCheck,
      },
    ]);
  } else if (options.noEmit) {
    addTask(execModule, ['tsc', args]);
  } else {
    addTask(execModule, ['tsc', args, {noErrCheck: options.noErrCheck}]);
  }
}

// Runs `vite build` (optionally in --watch mode) to transpile TS and produce
// the {frontend, engine, traceconv}_bundle.js files in cfg.outDistDir. All
// configuration lives in ui/vite.config.mjs; flags are passed via env vars to
// keep parity with the old rollup.config.js conventions.
//
// In watch+serve mode the frontend bundle is replaced by an in-process Vite
// dev server (see startViteDevServer) — workers and the service worker still
// go through `vite build --watch` because they're loaded as separate files by
// `new Worker(assetSrc(...))` / SW registration.
function runVite() {
  const baseEnv = {
    NO_SOURCE_MAPS: cfg.noSourceMaps ? 'true' : '',
    NO_TREESHAKE: cfg.noTreeshake ? 'true' : '',
    MINIFY_JS: cfg.minifyJs || '',
    IS_MEMORY64_ONLY: cfg.onlyWasmMemory64 ? 'true' : '',
  };
  const useDevServer = cfg.watch && cfg.startHttpServer;
  const bundles = ['engine', 'traceconv', 'service_worker', 'chrome_extension'];
  if (!useDevServer) bundles.unshift('frontend');
  if (cfg.bigtrace) bundles.push('bigtrace');
  if (cfg.openPerfettoTrace) bundles.push('open_perfetto_trace');
  for (const bundle of bundles) {
    const args = ['build', '--config', pjoin(ROOT_DIR, 'ui/vite.config.mjs')];
    if (cfg.watch) args.push('--watch');
    if (!cfg.verbose) args.push('--logLevel', 'warn');
    addTask(execModule, [
      'vite',
      args,
      {
        async: cfg.watch,
        env: {...baseEnv, BUNDLE: bundle},
        // Run Vite (and any tooling it spawns, e.g. vite-plugin-checker's
        // tsc) from ui/ so they pick up ui/tsconfig.json and resolve
        // node_modules naturally. build.mjs's global chdir to out/ui is
        // tailored to ninja/gn-style tasks, not to Vite's expectations.
        cwd: pjoin(ROOT_DIR, 'ui'),
      },
    ]);
  }
}

function genServiceWorkerManifestJson() {
  function makeManifest() {
    const manifest = {resources: {}};
    // When building the subresource manifest skip source maps, the manifest
    // itself and the copy of the index.html which is copied under /v1.2.3/.
    // The root /index.html will be fetched by service_worker.js separately.
    const skipRegex = /(\.map|manifest\.json|index.html)$/;
    walk(
      cfg.outDistDir,
      (absPath) => {
        const contents = fs.readFileSync(absPath);
        const relPath = path.relative(cfg.outDistDir, absPath);
        const b64 = crypto
          .createHash('sha256')
          .update(contents)
          .digest('base64');
        manifest.resources[relPath] = 'sha256-' + b64;
      },
      skipRegex,
    );
    const manifestJson = JSON.stringify(manifest, null, 2);
    fs.writeFileSync(pjoin(cfg.outDistDir, 'manifest.json'), manifestJson);
  }
  addTask(makeManifest, []);
}

// In dev (--watch --serve), Vite owns the user-facing port. It serves the
// frontend entry as native ESM transformed on the fly; everything else
// (wasm, fonts, css, /test/, /v1.2.3/-relative paths) is layered on as
// middleware. The custom HTTP server (startServer) is bypassed.
async function startViteDevServer() {
  // vite.config.mjs reads these at import time; set before createServer().
  if (cfg.onlyWasmMemory64) process.env.IS_MEMORY64_ONLY = 'true';
  if (cfg.noSourceMaps) process.env.NO_SOURCE_MAPS = 'true';
  if (cfg.noTreeshake) process.env.NO_TREESHAKE = 'true';
  if (cfg.minifyJs) process.env.MINIFY_JS = cfg.minifyJs;
  if (cfg.version) process.env.PERFETTO_DEV_VERSION = cfg.version;
  if (cfg.titleOverride) {
    process.env.PERFETTO_DEV_TITLE_OVERRIDE = cfg.titleOverride;
  }

  const {createServer} = await import('vite');
  const port = cfg.httpServerListenPort ?? DEFAULT_PORT;

  const headers = cfg.crossOriginIsolation
    ? {
        'Cross-Origin-Opener-Policy': 'same-origin',
        'Cross-Origin-Embedder-Policy': 'require-corp',
      }
    : undefined;

  const server = await createServer({
    configFile: pjoin(ROOT_DIR, 'ui/vite.config.mjs'),
    server: {
      host: cfg.httpServerListenHost,
      port,
      strictPort: false,
      headers,
      // Vite needs to read source files outside its root (ui/src/assets,
      // ui/src/gen via the symlink to out/, buildtools/, etc.).
      fs: {allow: [ROOT_DIR]},
    },
  });

  // Source HTML is served at / below; all patching now lives in
  // pluginPatchIndexHtml in ui/vite.config.mjs and runs via Vite's
  // transformIndexHtml pipeline.
  const indexSrc = pjoin(ROOT_DIR, 'ui/src/assets/index.html');

  // Serve /test/* from the repo (used by some e2e flows). Mirrors the
  // equivalent branch in startServer().
  server.middlewares.use((req, res, next) => {
    const url = req.url.split('?', 1)[0];
    if (!url.startsWith('/test/')) return next();
    const absPath = pjoin(ROOT_DIR, url);
    if (path.relative(ROOT_DIR, absPath).startsWith('..')) {
      res.statusCode = 403;
      return res.end('403');
    }
    fs.readFile(absPath, (err, data) => {
      if (err) {
        res.statusCode = 404;
        return res.end();
      }
      res.end(data);
    });
  });

  // frontend/index.ts inserts `<link rel="stylesheet" href="frontend.css">`
  // at runtime, which is needed in prod but in dev the styles come from the
  // SCSS module that Vite transforms inline. Return a 200 empty body so the
  // link's onload fires and init can proceed.
  server.middlewares.use((req, res, next) => {
    const url = req.url.split('?', 1)[0];
    if (url === '/frontend.css' || url.endsWith('/frontend.css')) {
      res.setHeader('Content-Type', 'text/css');
      return res.end('/* dev stub: styles injected by Vite */');
    }
    next();
  });

  // Fall back to serving files from outDistRootDir / outDistDir for anything
  // Vite hasn't claimed (wasm modules, fonts under /assets/, manifest.json,
  // etc.). In prod, frontend.css lives inside the versioned dir, so the
  // url('assets/Roboto.woff2') paths in typefaces.scss resolve to
  // /<version>/assets/...; in dev the stylesheet is injected from /, so the
  // browser asks for /assets/Roboto.woff2. Try outDistDir as a second root
  // so those font requests succeed.
  server.middlewares.use((req, res, next) => {
    const url = req.url.split('?', 1)[0];
    // Vite handles JS/TS/CSS modules and HMR endpoints itself.
    if (url === '/' || url === '/index.html') return next();
    const roots = [cfg.outDistRootDir, cfg.outDistDir];
    const tryRoot = (i) => {
      if (i >= roots.length) return next();
      const root = roots[i];
      const absPath = path.normalize(pjoin(root, url));
      if (path.relative(root, absPath).startsWith('..')) return tryRoot(i + 1);
      fs.stat(absPath, (err, stat) => {
        if (err || !stat.isFile()) return tryRoot(i + 1);
        fs.readFile(absPath, (rerr, data) => {
          if (rerr) return tryRoot(i + 1);
          const ext = url.split('.').pop();
          const mime =
            {
              wasm: 'application/wasm',
              woff2: 'font/woff2',
              png: 'image/png',
              json: 'application/json',
              css: 'text/css',
              js: 'application/javascript',
              html: 'text/html',
            }[ext] || 'application/octet-stream';
          res.setHeader('Content-Type', mime);
          res.end(data);
        });
      });
    };
    tryRoot(0);
  });

  // Last: serve the patched index.html at / (and any other unmatched route,
  // mirroring the SPA convention).
  server.middlewares.use(async (req, res, next) => {
    const url = req.url.split('?', 1)[0];
    if (url !== '/' && url !== '/index.html') return next();
    try {
      const raw = fs.readFileSync(indexSrc, 'utf8');
      const transformed = await server.transformIndexHtml(url, raw);
      res.setHeader('Content-Type', 'text/html');
      res.end(transformed);
    } catch (e) {
      next(e);
    }
  });

  await server.listen();
  server.printUrls();
  // Make sure the dev server is shut down on process exit.
  subprocesses.push({pid: 'vite-dev', kill: () => server.close()});
}

function isDistComplete() {
  // In watch+serve mode the frontend bundle and its CSS are served live by
  // the Vite dev server, never materialised on disk. Only require the
  // artifacts that genuinely have to exist before the user can load a trace.
  const useDevServer = cfg.watch && cfg.startHttpServer;
  const requiredArtifacts = [
    ...(useDevServer ? [] : ['frontend_bundle.js', 'frontend.css']),
    'engine_bundle.js',
    'traceconv_bundle.js',
    ...cfg.wasmModules.map((wasmMod) => `${wasmMod}.wasm`),
  ];
  const relPaths = new Set();
  walk(cfg.outDistDir, (absPath) => {
    relPaths.add(path.relative(cfg.outDistDir, absPath));
  });
  for (const fName of requiredArtifacts) {
    if (!relPaths.has(fName)) return false;
  }
  return true;
}

function copyExtensionAssets() {
  addTask(cp, [
    pjoin(ROOT_DIR, 'ui/src/assets/logo-128.png'),
    pjoin(cfg.outExtDir, 'logo-128.png'),
  ]);
  addTask(cp, [
    pjoin(ROOT_DIR, 'ui/src/chrome_extension/manifest.json'),
    pjoin(cfg.outExtDir, 'manifest.json'),
  ]);
}

// -----------------------
// Task chaining functions
// -----------------------

function addTask(func, args) {
  const task = new Task(func, args);
  for (const t of tasks) {
    if (t.identity === task.identity) {
      return;
    }
  }
  tasks.push(task);
  setTimeout(runTasks, 0);
}

function runTasks() {
  const snapTasks = tasks.splice(0); // snap = std::move(tasks).
  tasksTot += snapTasks.length;
  for (const task of snapTasks) {
    const DIM = '\u001b[2m';
    const BRT = '\u001b[37m';
    const RST = '\u001b[0m';
    const ms = (performance.now() - tStart) / 1000;
    const ts = `[${DIM}${ms.toFixed(3)}${RST}]`;
    const descr = task.description.substr(0, 80);
    console.log(`${ts} ${BRT}${++tasksRan}/${tasksTot}${RST}\t${descr}`);
    task.func.apply(/* this=*/ undefined, task.args);
  }
}

// Executes all the RULES that match the given |absPath|.
function scanFile(absPath) {
  console.assert(fs.existsSync(absPath));
  console.assert(path.isAbsolute(absPath));
  const normPath = path.relative(ROOT_DIR, absPath);
  for (const rule of RULES) {
    const match = rule.r.exec(normPath);
    if (!match || match[0] !== normPath) continue;
    const captureGroup = match.length > 1 ? match[1] : undefined;
    rule.f(absPath, captureGroup);
  }
}

// Walks the passed |dir| recursively and, for each file, invokes the matching
// RULES. If --watch is used, it also installs a fswatch() and re-triggers the
// matching RULES on each file change.
function scanDir(dir, regex) {
  const filterFn = regex ? (absPath) => regex.test(absPath) : () => true;
  const absDir = path.isAbsolute(dir) ? dir : pjoin(ROOT_DIR, dir);
  // Add a fs watch if in watch mode.
  if (cfg.watch) {
    fs.watch(absDir, {recursive: true}, (_eventType, relFilePath) => {
      const filePath = pjoin(absDir, relFilePath);
      if (!filterFn(filePath)) return;
      if (cfg.verbose) {
        console.log('File change detected', _eventType, filePath);
      }
      if (fs.existsSync(filePath)) {
        scanFile(filePath, filterFn);
      }
    });
  }
  walk(absDir, (f) => {
    if (filterFn(f)) scanFile(f);
  });
}

function exec(cmd, args, opts) {
  opts = opts || {};
  opts.stdout = opts.stdout || 'inherit';
  if (cfg.verbose) console.log(`${cmd} ${args.join(' ')}\n`);
  const spwOpts = {
    cwd: opts.cwd || cfg.outDir,
    stdio: ['ignore', opts.stdout, 'inherit'],
  };
  if (opts.env) {
    spwOpts.env = {...process.env, ...opts.env};
  }
  const checkExitCode = (code, signal) => {
    if (signal === 'SIGINT' || signal === 'SIGTERM') return;
    if (code !== 0 && !opts.noErrCheck) {
      console.error(`${cmd} ${args.join(' ')} failed with code ${code}`);
      process.exit(1);
    }
  };
  if (opts.async) {
    const proc = childProcess.spawn(cmd, args, spwOpts);
    const procIndex = subprocesses.length;
    subprocesses.push(proc);
    return new Promise((resolve, _reject) => {
      proc.on('exit', (code, signal) => {
        delete subprocesses[procIndex];
        checkExitCode(code, signal);
        resolve();
      });
    });
  } else {
    const spawnRes = childProcess.spawnSync(cmd, args, spwOpts);
    checkExitCode(spawnRes.status, spawnRes.signal);
    return spawnRes;
  }
}

function execModule(module, args, opts) {
  const modPath = pjoin(ROOT_DIR, 'ui/node_modules/.bin', module);
  return exec(modPath, args || [], opts);
}

// ------------------------------------------
// File system & subprocess utility functions
// ------------------------------------------

class Task {
  constructor(func, args) {
    this.func = func;
    this.args = args || [];
    // |identity| is used to dedupe identical tasks in the queue.
    this.identity = JSON.stringify([this.func.name, this.args]);
  }

  get description() {
    const ret = this.func.name.startsWith('exec') ? [] : [this.func.name];
    const flattenedArgs = [].concat(...this.args);
    for (const arg of flattenedArgs) {
      if (typeof arg === 'object' && arg !== null) {
        ret.push(JSON.stringify(arg));
        continue;
      }
      const argStr = `${arg}`;
      if (argStr.startsWith('/')) {
        ret.push(path.relative(cfg.outDir, arg));
      } else {
        ret.push(argStr);
      }
    }
    return ret.join(' ');
  }
}

function walk(dir, callback, skipRegex) {
  for (const child of fs.readdirSync(dir)) {
    const childPath = pjoin(dir, child);
    const stat = fs.lstatSync(childPath);
    if (skipRegex !== undefined && skipRegex.test(child)) continue;
    if (stat.isDirectory()) {
      walk(childPath, callback, skipRegex);
    } else if (!stat.isSymbolicLink()) {
      callback(childPath);
    }
  }
}

// Recursively build a list of files in a given directory and return a list of
// file paths, similar to `find -type f`.
function listFilesRecursive(dir) {
  const fileList = [];

  walk(dir, (filePath) => {
    fileList.push(filePath);
  });

  return fileList;
}

function ensureDir(dirPath, clean) {
  const exists = fs.existsSync(dirPath);
  if (exists && clean) {
    console.log('rm', dirPath);
    fs.rmSync(dirPath, {recursive: true});
  }
  if (!exists || clean) fs.mkdirSync(dirPath, {recursive: true});
  return dirPath;
}

function cp(src, dst) {
  ensureDir(path.dirname(dst));
  if (cfg.verbose) {
    console.log(
      'cp',
      path.relative(ROOT_DIR, src),
      '->',
      path.relative(ROOT_DIR, dst),
    );
  }
  fs.copyFileSync(src, dst);
}

function mklink(src, dst) {
  // If the symlink already points to the right place don't touch it. This is
  // to avoid changing the mtime of the ui/ dir when unnecessary.
  if (fs.existsSync(dst)) {
    if (fs.lstatSync(dst).isSymbolicLink() && fs.readlinkSync(dst) === src) {
      return;
    } else {
      fs.unlinkSync(dst);
    }
  }
  fs.symlinkSync(src, dst);
}

function prepareBuildLock() {
  if (fs.existsSync(cfg.lockFile)) {
    const oldPid = fs.readFileSync(cfg.lockFile, 'utf8').trim();
    let running = true;
    try {
      // Check if oldPid exists.
      process.kill(parseInt(oldPid), 0);
    } catch (e) {
      running = false;
    }
    if (running) {
      console.error(
        `Error: a build.mjs instance is already running (${cfg.lockFile} PID=${oldPid}).`,
      );
      console.error(
        'Hint: use --no-build (-n) to skip the build and avoid the lock.',
      );
      process.exit(1);
    } else {
      console.log(`Removing stale lock file for PID ${oldPid}`);
      fs.unlinkSync(cfg.lockFile);
    }
  }
  fs.writeFileSync(cfg.lockFile, process.pid.toString());
  process.on('exit', () => releaseBuildLock());
}

function releaseBuildLock() {
  if (fs.existsSync(cfg.lockFile)) {
    const pid = fs.readFileSync(cfg.lockFile, 'utf8').trim();
    if (pid === process.pid.toString()) {
      fs.unlinkSync(cfg.lockFile);
    } else {
      console.warn(`Ignoring stale lock file PID ${pid}`);
    }
  }
}

main();
