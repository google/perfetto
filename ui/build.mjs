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
import {ensureDir} from './scripts/fs_utils.mjs';
import {compileProtos, prebuild} from './scripts/prebuild.mjs';
import {startStaticServer} from './scripts/static_server.mjs';
import {runInProcStep, runStep} from './scripts/steps.mjs';
import {
  ALL_BUNDLES,
  WORKER_BUNDLES,
  viteBuild,
  viteDev,
} from './scripts/vite_runner.mjs';
import {startVitest} from 'vitest/node';

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

  switch (args.command) {
    case 'pre':
      await prebuild({rootDir: ROOT_DIR, outDir, version: VERSION});
      break;
    case 'build':
      await prebuild({rootDir: ROOT_DIR, outDir, version: VERSION});
      await viteBuild({rootDir: ROOT_DIR, bundles: ALL_BUNDLES});
      break;
    case 'dev':
      await prebuild({rootDir: ROOT_DIR, outDir, version: VERSION});
      // Worker bundles are loaded via `new Worker(url)` at runtime, so they
      // need to exist as real files in dist/v<version>/ in both dev and prod.
      await viteBuild({rootDir: ROOT_DIR, bundles: WORKER_BUNDLES});
      await viteDev({
        rootDir: ROOT_DIR,
        outDir,
        version: VERSION,
        port: DEFAULT_PORT,
      });
      break;
    case 'preview':
      startStaticServer({
        rootDir: ROOT_DIR,
        distRootDir: pjoin(outDir, 'dist'),
        host: '127.0.0.1',
        defaultPort: DEFAULT_PORT,
        portWasExplicit: false,
        crossOriginIsolation: false,
      });
      break;
    case 'test':
      await prebuild({rootDir: ROOT_DIR, outDir, version: VERSION});
      await runUnitTests();
      break;
    case 'typecheck': {
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

async function runUnitTests() {
  const prevCwd = process.cwd();
  process.chdir(pjoin(ROOT_DIR, 'ui'));
  try {
    await runInProcStep('vitest', async () => {
      const vitest = await startVitest('test', [], {
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
