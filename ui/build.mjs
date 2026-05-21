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

// Entry point for the Perfetto UI build. Subcommands:
//   pre        Wasm + protos + asset copy (everything Vite consumes).
//   build      pre + `vite build` for the production bundles.
//   dev        pre + in-process Vite dev server with HMR.
//   preview    Static HTTP server over an already-built dist/.
//   test       pre + Vitest.
//   typecheck  Just enough prebuild to run tsc --noEmit.
//
// Vite owns TS transpile, bundling, and dev-time HMR (see ui/vite.config.mjs).
// This script glues together the surrounding tasks; the heavy lifting lives
// under ui/scripts/.

import argparse from 'argparse';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {startVitest} from 'vitest/node';
import {acquireBuildLock, warnIfBuildLockHeld} from './scripts/build_lock.mjs';
import {ensureDir} from './scripts/fs_utils.mjs';
import {
  compileProtos,
  genServiceWorkerManifestJson,
  prebuild,
} from './scripts/prebuild.mjs';
import {startStaticServer} from './scripts/static_server.mjs';
import {runInProcStep, runStep} from './scripts/steps.mjs';
import {
  ALL_BUNDLES,
  OPEN_PERFETTO_TRACE_BUNDLE,
  WORKER_BUNDLES,
  viteBuild,
  viteDev,
} from './scripts/vite_runner.mjs';

const pjoin = path.join;
const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const ROOT_DIR = path.dirname(__dirname); // The repo root.
const DEFAULT_PORT = 10000;

// Compute the build version once and export to env so vite.config.mjs picks
// it up. All bundles and static assets live under dist/v<version>/.
const VERSION = spawnSync(
  'python3',
  [pjoin(ROOT_DIR, 'tools/write_version_header.py'), '--stdout'],
  {encoding: 'utf8'},
).stdout.trim();
if (!VERSION) throw new Error('Failed to compute UI version');
process.env.PERFETTO_UI_VERSION = VERSION;

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
    PERFETTO_UI_OUT=/tmp/perfetto-out
    PERFETTO_UI_NO_BUILD=1
    PERFETTO_UI_SERVE_HOST=0.0.0.0
    PERFETTO_UI_TITLE=my-instance

  Defaults can also be persisted in:
    ~/.config/perfetto/ui-dev-server.env
  (one KEY=VALUE per line, # comments supported). Shell env vars take
  precedence over the file.
`,
  });
  parser.add_argument('--out', {help: 'Output directory'});
  parser.add_argument('--no-build', '-n', {
    action: 'store_true',
    help: 'Skip prebuild (wasm/protos/assets) — useful for fast iteration',
  });
  parser.add_argument('--no-wasm', '-W', {
    action: 'store_true',
    help: 'Skip the ninja wasm build (assumes outputs already exist)',
  });
  parser.add_argument('--no-depscheck', {
    action: 'store_true',
    help: 'Skip install-build-deps check',
  });
  parser.add_argument('--no-override-gn-args', {
    action: 'store_true',
    help: "Don't auto-set gn args (preserves manual configuration)",
  });
  parser.add_argument('--debug', '-d', {
    action: 'store_true',
    help: 'Debug wasm build (is_debug=true, copies .wasm.map files)',
  });
  parser.add_argument('--only-wasm-memory64', {
    action: 'store_true',
    help: 'Skip the non-memory64 trace_processor wasm build',
  });
  parser.add_argument('--minify-js', {
    choices: ['preserve_comments', 'all'],
    help: 'Minify JS bundles',
  });
  parser.add_argument('--no-source-maps', {
    action: 'store_true',
    help: 'Skip source map generation',
  });
  parser.add_argument('--no-treeshake', {
    action: 'store_true',
    help: 'Disable rollup tree-shaking (faster incremental rebuilds)',
  });
  parser.add_argument('--cross-origin-isolation', {
    action: 'store_true',
    help: 'Send COOP/COEP headers (needed for SharedArrayBuffer)',
  });
  parser.add_argument('--serve-host', {help: 'dev/preview bind host'});
  parser.add_argument('--serve-port', {
    type: 'int',
    help: 'dev/preview bind port',
  });
  parser.add_argument('--title', {help: 'Override <title> tag'});
  parser.add_argument('--open-perfetto-trace', {
    action: 'store_true',
    help: 'Also build the open_perfetto_trace bundle',
  });
  parser.add_argument('--test-filter', '-f', {
    help: "filter tests by pattern, e.g. 'chrome_render'",
  });
  parser.add_argument('--interactive', '-i', {
    action: 'store_true',
    help: 'Run playwright tests in interactive mode',
  });
  parser.add_argument('--rebaseline', '-r', {
    action: 'store_true',
    help: 'Rebaseline screenshot tests',
  });

  const sub = parser.add_subparsers({dest: 'command'});
  sub.add_parser('pre', {help: 'Run pre-build steps and exit'});
  sub.add_parser('build', {help: 'Build for production'});
  sub.add_parser('dev', {help: 'Start the dev server'});
  sub.add_parser('preview', {help: 'Preview production build'});
  sub.add_parser('test', {help: 'Run unit tests'});
  sub.add_parser('typecheck', {help: 'Run type checking only'});

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

  const outRootDir = path.resolve(args.out || pjoin(ROOT_DIR, 'out/ui'));
  const outDir = ensureDir(pjoin(outRootDir, 'ui'));

  // Plumb bundler-shaping flags through to vite.config.mjs via env. Set them
  // unconditionally so they cover both `vite build` and the in-process dev
  // server (which reads env at config import time).
  if (args.no_source_maps) process.env.NO_SOURCE_MAPS = 'true';
  if (args.no_treeshake) process.env.NO_TREESHAKE = 'true';
  if (args.minify_js) process.env.MINIFY_JS = args.minify_js;
  if (args.only_wasm_memory64) process.env.IS_MEMORY64_ONLY = 'true';
  if (args.open_perfetto_trace) process.env.ENABLE_OPEN_PERFETTO_TRACE = 'true';
  if (args.title) process.env.PERFETTO_DEV_TITLE_OVERRIDE = args.title;
  // Test-runner shaping: pickup-by-env-var convention preserved from main.
  if (args.interactive) process.env.PERFETTO_UI_TESTS_INTERACTIVE = '1';
  if (args.rebaseline) process.env.PERFETTO_UI_TESTS_REBASELINE = '1';

  const prebuildOpts = {
    rootDir: ROOT_DIR,
    outDir,
    version: VERSION,
    debug: args.debug,
    skipWasm: args.no_wasm,
    skipDepscheck: args.no_depscheck,
    noOverrideGnArgs: args.no_override_gn_args,
    onlyWasmMemory64: args.only_wasm_memory64,
    titleOverride: args.title || '',
  };

  const serverHost = args.serve_host || '127.0.0.1';
  const serverPort = args.serve_port;
  const portWasExplicit = serverPort !== undefined;

  // Bundles to feed `vite build`. Open-perfetto-trace is opt-in; bigtrace is
  // intentionally not wired in this version of the script.
  const bundlesForProd = [...ALL_BUNDLES];
  if (args.open_perfetto_trace) bundlesForProd.push(OPEN_PERFETTO_TRACE_BUNDLE);

  // Lock policy: anything that needs outDir to stay stable acquires the
  // build lock for its lifetime.
  //   pre/build/test: acquire iff they're going to wipe (i.e. !--no-build).
  //   dev/preview:    always acquire — they hold outDir live, --no-build or not.
  //   typecheck:      doesn't touch outDir/dist, no lock.
  const wipingCommands = ['pre', 'build', 'test'];
  const serverCommands = ['dev', 'preview'];
  if (
    (wipingCommands.includes(args.command) && !args.no_build) ||
    serverCommands.includes(args.command)
  ) {
    acquireBuildLock({outDir});
  } else if (args.no_build && wipingCommands.includes(args.command)) {
    warnIfBuildLockHeld({outDir});
  }

  switch (args.command) {
    case 'pre':
      await prebuild(prebuildOpts);
      break;
    case 'build':
      await prebuild(prebuildOpts);
      await viteBuild({rootDir: ROOT_DIR, bundles: bundlesForProd});
      await runInProcStep('service worker manifest', () =>
        genServiceWorkerManifestJson({
          distDir: pjoin(outDir, 'dist', VERSION),
        }),
      );
      break;
    case 'dev':
      if (!args.no_build) {
        await prebuild(prebuildOpts);
        // Worker bundles are loaded via `new Worker(url)` at runtime, so they
        // need to exist as real files in dist/v<version>/ in dev too.
        await viteBuild({rootDir: ROOT_DIR, bundles: WORKER_BUNDLES});
      }
      await viteDev({
        rootDir: ROOT_DIR,
        outDir,
        version: VERSION,
        host: serverHost,
        port: serverPort ?? DEFAULT_PORT,
        crossOriginIsolation: args.cross_origin_isolation,
      });
      break;
    case 'preview':
      startStaticServer({
        rootDir: ROOT_DIR,
        distRootDir: pjoin(outDir, 'dist'),
        host: serverHost,
        port: serverPort,
        defaultPort: DEFAULT_PORT,
        portWasExplicit,
        crossOriginIsolation: args.cross_origin_isolation,
      });
      break;
    case 'test':
      if (!args.no_build) await prebuild(prebuildOpts);
      await runUnitTests({testFilter: args.test_filter});
      break;
    case 'typecheck': {
      // typecheck only touches tsc/gen, not outDir/dist — safe to coexist
      // with a running dev/preview, so no lock needed.
      const genDir = ensureDir(pjoin(outDir, 'tsc/gen'));
      await compileProtos({rootDir: ROOT_DIR, genDir});
      await runStep(
        'tsc --noEmit',
        pjoin(ROOT_DIR, 'ui/node_modules/.bin/tsc'),
        ['--noEmit'],
        {cwd: pjoin(ROOT_DIR, 'ui')},
      );
      break;
    }
    default:
      throw new Error(`Unknown command: ${args.command}`);
  }
}

async function runUnitTests({testFilter} = {}) {
  const prevCwd = process.cwd();
  process.chdir(pjoin(ROOT_DIR, 'ui'));
  try {
    await runInProcStep('vitest', async () => {
      const cliFilters = testFilter ? [testFilter] : [];
      const vitest = await startVitest('test', cliFilters, {
        config: pjoin(ROOT_DIR, 'ui/vitest.config.mjs'),
        watch: false,
      });
      const failed = vitest?.state.getCountOfFailedTests() ?? 0;
      await vitest?.close();
      if (failed > 0) throw new Error(`${failed} test(s) failed`);
    });
  } catch (e) {
    process.chdir(prevCwd);
    process.exit(1);
  }
  process.chdir(prevCwd);
}

main();
