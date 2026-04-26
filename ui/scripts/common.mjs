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

import {spawnSync, spawn} from 'node:child_process';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import fs from 'node:fs';

const SCRIPTS_DIR = dirname(fileURLToPath(import.meta.url));
export const UI_DIR = dirname(SCRIPTS_DIR);
export const ROOT_DIR = dirname(UI_DIR);

export function ensureDir(dir, {clean = false} = {}) {
  if (clean && fs.existsSync(dir)) {
    fs.rmSync(dir, {recursive: true, force: true});
  }
  fs.mkdirSync(dir, {recursive: true});
  return dir;
}

export function cp(src, dst) {
  ensureDir(dirname(dst));
  fs.copyFileSync(src, dst);
}

export function mklink(target, linkPath) {
  if (fs.existsSync(linkPath)) {
    if (
      fs.lstatSync(linkPath).isSymbolicLink() &&
      fs.readlinkSync(linkPath) === target
    ) {
      return;
    }
    fs.unlinkSync(linkPath);
  }
  fs.symlinkSync(target, linkPath);
}

export function walk(dir, callback, {skipRegex} = {}) {
  if (!fs.existsSync(dir)) return;
  for (const child of fs.readdirSync(dir)) {
    if (skipRegex && skipRegex.test(child)) continue;
    const childPath = join(dir, child);
    const stat = fs.lstatSync(childPath);
    if (stat.isDirectory()) {
      walk(childPath, callback, {skipRegex});
    } else if (!stat.isSymbolicLink()) {
      callback(childPath);
    }
  }
}

// Synchronously invoke a command. Exits the process on non-zero return.
// When `args` is very long (e.g. stdlib SQL file lists), the logged command
// is truncated to keep the output readable; the full argv still runs.
export function exec(cmd, args, {cwd, env, silent = false} = {}) {
  if (!silent) {
    const line = `$ ${cmd} ${args.join(' ')}`;
    console.log(line.length > 400 ? line.slice(0, 400) + ` … [${args.length} args]` : line);
  }
  const res = spawnSync(cmd, args, {
    cwd: cwd ?? UI_DIR,
    env: env ?? process.env,
    stdio: silent ? ['ignore', 'pipe', 'pipe'] : 'inherit',
  });
  if (res.status !== 0) {
    if (silent) {
      process.stderr.write(res.stderr ?? '');
      process.stdout.write(res.stdout ?? '');
    }
    throw new Error(
      `${cmd} ${args.join(' ')} exited with status ${res.status}`,
    );
  }
  return res;
}

// Spawn in the background. Returns a ChildProcess.
export function spawnBg(cmd, args, {cwd, env} = {}) {
  console.log(`$ ${cmd} ${args.join(' ')}  (background)`);
  const proc = spawn(cmd, args, {
    cwd: cwd ?? UI_DIR,
    env: env ?? process.env,
    stdio: 'inherit',
  });
  return proc;
}

export function execCapture(cmd, args, {cwd, env} = {}) {
  const res = spawnSync(cmd, args, {
    cwd: cwd ?? UI_DIR,
    env: env ?? process.env,
    encoding: 'utf8',
  });
  if (res.status !== 0) {
    process.stderr.write(res.stderr ?? '');
    throw new Error(
      `${cmd} ${args.join(' ')} exited with status ${res.status}`,
    );
  }
  return res.stdout;
}

// Resolve the output directory using the same rules as CLAUDE.md:
// - If $OUT is set, use it (relative to ROOT_DIR unless absolute).
// - Else default to ROOT_DIR/out/ui.
export function resolveOutDir(explicit) {
  if (explicit) return resolve(explicit);
  if (process.env.OUT) return resolve(ROOT_DIR, process.env.OUT);
  return join(ROOT_DIR, 'out/ui');
}

// Read the current UI version (e.g. "v54.0-991e50e94").
export function readVersion() {
  const out = execCapture('python3', [
    join(ROOT_DIR, 'tools/write_version_header.py'),
    '--stdout',
  ]);
  return out.trim();
}

// Derives all output subdirectories from a given root outDir.
// Matches the layout of the old ui/build.js, except that the generated
// files now live directly under `ui/src/gen/` (a real directory) instead of
// being a symlink into out/. This sidesteps node_modules resolution
// surprises with pnpm-style symlinks.
export function makeLayout(outDir, version) {
  const outUiDir = join(outDir, 'ui');
  const outDistRootDir = join(outUiDir, 'dist');
  const outDistDir = join(outDistRootDir, version);
  return {
    outDir,
    outUiDir,
    outTscDir: join(outUiDir, 'tsc'),
    // All codegen + wasm glue land here, which is now a real source dir.
    outGenDir: join(UI_DIR, 'src/gen'),
    outDistRootDir,
    outDistDir,
    outExtDir: join(outUiDir, 'chrome_extension'),
    outBigtraceDistDir: join(outDistDir, 'bigtrace'),
    outOpenPerfettoTraceDistDir: join(outDistRootDir, 'open_perfetto_trace'),
    outUiTestArtifactsDir: join(outDir, 'ui-test-artifacts'),
    outWasmDir: join(outDir, 'wasm'),
    outWasmMemory64Dir: join(outDir, 'wasm_memory64'),
  };
}

// Symlinks that several parts of the pipeline rely on:
//   ui/out              -> <outUiDir>  (convenience for `ls ui/out/…`)
//   <outDir>/test/data  -> <ROOT_DIR>/test/data  (for playwright fixtures)
// The old build also maintained:
//   ui/src/gen -> <outGenDir>
//   ui/node_modules -> <outTscDir>/node_modules
//   <outUiDir>/dist_version -> ./dist/<version>
// These are no longer needed: ui/src/gen is now a real directory, and under
// Vite the other two symlinks are unused.
export function ensureSymlinks(layout) {
  ensureDir(layout.outUiDir);
  ensureDir(layout.outTscDir);
  ensureDir(layout.outGenDir);
  ensureDir(layout.outDistDir);

  // Replace any stale ui/src/gen -> out/… symlink with a real directory.
  const genPath = join(UI_DIR, 'src/gen');
  if (fs.existsSync(genPath) && fs.lstatSync(genPath).isSymbolicLink()) {
    fs.unlinkSync(genPath);
    ensureDir(genPath);
  }

  mklink(layout.outUiDir, join(UI_DIR, 'out'));

  const testDir = ensureDir(join(layout.outDir, 'test'));
  mklink(join(ROOT_DIR, 'test/data'), join(testDir, 'data'));
}

export function fileExists(p) {
  try {
    fs.accessSync(p);
    return true;
  } catch {
    return false;
  }
}

// Loads ~/.config/perfetto/ui-dev-server.env (if present) and injects any
// KEY=VALUE pairs into process.env, without overriding variables already
// set in the environment. Ported verbatim from the old ui/build.js.
export function loadDevServerEnvFile() {
  const home = process.env.HOME || process.env.USERPROFILE || '';
  if (!home) return;
  const envFile = join(home, '.config', 'perfetto', 'ui-dev-server.env');
  let content;
  try {
    content = fs.readFileSync(envFile, 'utf8');
  } catch {
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
