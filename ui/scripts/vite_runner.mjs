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

// Vite orchestration: production bundle builds and in-process dev server.

import fs from 'node:fs';
import path from 'node:path';
import * as vite from 'vite';
import {runInProcStep} from './steps.mjs';

const pjoin = path.join;

export const ALL_BUNDLES = [
  'frontend',
  'engine',
  'traceconv',
  'service_worker',
  'chrome_extension',
];

// Worker bundles need a real .js file on disk even in dev — they're loaded
// via `new Worker(assetSrc(...))`, which the Vite dev server's module graph
// doesn't intercept.
export const WORKER_BUNDLES = ['engine', 'traceconv'];

// Optional bundles, only built when the corresponding flag is passed.
export const OPEN_PERFETTO_TRACE_BUNDLE = 'open_perfetto_trace';

// Runs `vite build` in-process for each named bundle. The config file
// (ui/vite.config.mjs) selects its input from process.env.BUNDLE, so we set
// that before each call. Bundles are built sequentially: vite-plugin-checker
// runs tsc in the frontend bundle and we don't want N copies of it racing.
export async function viteBuild({rootDir, bundles, mode}) {
  const configFile = pjoin(rootDir, 'ui/vite.config.mjs');
  // Vite (and any tooling it spawns, e.g. vite-plugin-checker's tsc) expects
  // the cwd to be ui/ so it picks up ui/tsconfig.json and resolves
  // node_modules naturally. Restore the previous cwd when done.
  const prevCwd = process.cwd();
  process.chdir(pjoin(rootDir, 'ui'));
  try {
    for (const bundle of bundles) {
      process.env.BUNDLE = bundle;
      await runInProcStep(`vite build (${bundle})`, () =>
        vite.build({configFile, mode, logLevel: 'warn'}),
      );
    }
  } finally {
    process.chdir(prevCwd);
  }
}

// Starts the Vite dev server in-process. Vite owns the user-facing port,
// serves frontend/index.ts as native ESM transformed on the fly, and runs
// HMR. We layer a few middlewares on top:
//   - /test/* serves files from the repo (used by some e2e flows).
//   - /frontend.css returns an empty 200 because frontend/index.ts inserts
//     a <link rel=stylesheet> for it at runtime (needed in prod); in dev the
//     styles come from the SCSS module that Vite transforms inline.
//   - Anything Vite doesn't claim falls back to outDir/dist/v<version>/
//     (wasm modules, fonts under /assets/, etc.).
//   - / and /index.html serve the patched ui/src/assets/index.html via
//     server.transformIndexHtml so pluginPatchIndexHtml fires.
export async function viteDev({
  rootDir,
  outDir,
  version,
  host = '127.0.0.1',
  port,
  crossOriginIsolation = false,
}) {
  // Static files (wasm, fonts, etc.) live under dist/v<version>/. We serve
  // them at root-relative URLs in dev because the patched index.html sets
  // version='.' (see pluginPatchIndexHtml).
  const distRootDir = pjoin(outDir, 'dist', version);
  const indexSrc = pjoin(rootDir, 'ui/src/assets/index.html');
  const prevCwd = process.cwd();
  process.chdir(pjoin(rootDir, 'ui'));
  let server;
  try {
    const headers = crossOriginIsolation
      ? {
          'Cross-Origin-Opener-Policy': 'same-origin',
          'Cross-Origin-Embedder-Policy': 'require-corp',
        }
      : undefined;
    server = await vite.createServer({
      configFile: pjoin(rootDir, 'ui/vite.config.mjs'),
      server: {
        host,
        port,
        strictPort: false,
        headers,
        fs: {allow: [rootDir]},
      },
    });
  } finally {
    process.chdir(prevCwd);
  }

  server.middlewares.use((req, res, next) => {
    const url = req.url.split('?', 1)[0];
    if (!url.startsWith('/test/')) return next();
    const absPath = pjoin(rootDir, url);
    if (path.relative(rootDir, absPath).startsWith('..')) {
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

  server.middlewares.use((req, res, next) => {
    const url = req.url.split('?', 1)[0];
    if (url === '/frontend.css' || url.endsWith('/frontend.css')) {
      res.setHeader('Content-Type', 'text/css');
      return res.end('/* dev stub: styles injected by Vite */');
    }
    next();
  });

  server.middlewares.use((req, res, next) => {
    const url = req.url.split('?', 1)[0];
    if (url === '/' || url === '/index.html') return next();
    const absPath = path.normalize(pjoin(distRootDir, url));
    if (path.relative(distRootDir, absPath).startsWith('..')) return next();
    fs.stat(absPath, (err, stat) => {
      if (err || !stat.isFile()) return next();
      fs.readFile(absPath, (rerr, data) => {
        if (rerr) return next();
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
  });

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
  process.on('SIGINT', () => server.close().then(() => process.exit(0)));
  process.on('SIGTERM', () => server.close().then(() => process.exit(0)));
}
