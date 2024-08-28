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

'use strict';

// This script builds the perfetto.dev docs website.

const argparse = require('argparse');
const child_process = require('child_process');
const fs = require('fs');
const http = require('http');
const path = require('path');
const pjoin = path.join;

const ROOT_DIR = path.dirname(path.dirname(__dirname));  // The repo root.

const cfg = {
  watch: false,
  verbose: false,
  startHttpServer: false,

  outDir: pjoin(ROOT_DIR, 'out/perfetto.dev'),
};

function main() {
  const parser = new argparse.ArgumentParser();
  parser.add_argument('--out', {help: 'Output directory'});
  parser.add_argument('--watch', '-w', {action: 'store_true'});
  parser.add_argument('--serve', '-s', {action: 'store_true'});
  parser.add_argument('--verbose', '-v', {action: 'store_true'});

  const args = parser.parse_args();
  cfg.outDir = path.resolve(ensureDir(args.out || cfg.outDir, /*clean=*/ true));
  cfg.watch = !!args.watch;
  cfg.verbose = !!args.verbose;
  cfg.startHttpServer = args.serve;

  // Check that deps are current before starting.
  const installBuildDeps = pjoin(ROOT_DIR, 'tools/install-build-deps');

  // --filter=nodejs --filter=pnpm --filter=gn --filter=ninja is to match what
  // cloud_build_entrypoint.sh passes to install-build-deps. It doesn't bother
  // installing the full toolchains because, unlike the Perfetto UI, it doesn't
  // need Wasm.
  const depsArgs = [
    '--check-only=/dev/null',
    '--ui',
    '--filter=nodejs',
    '--filter=pnpm',
    '--filter=gn',
    '--filter=ninja'
  ];
  exec(installBuildDeps, depsArgs);

  ninjaBuild();

  if (args.watch) {
    watchDir('docs');
    watchDir('infra/perfetto.dev/src/assets');
    watchDir('protos');
    watchDir('python');
    watchDir('src/trace_processor/tables');
  }
  if (args.serve) {
    startServer();
  }
}

function ninjaBuild() {
  exec(
      pjoin(ROOT_DIR, 'tools/gn'),
      ['gen', cfg.outDir, '--args=enable_perfetto_site=true']);
  exec(pjoin(ROOT_DIR, 'tools/ninja'), ['-C', cfg.outDir, 'site']);
}

function startServer() {
  const port = 8082;
  console.log(`Starting HTTP server on http://localhost:${port}`)
  const serveDir = path.join(cfg.outDir, 'site');
  http.createServer(function(req, res) {
        console.debug(req.method, req.url);
        let uri = req.url.split('?', 1)[0];
        uri += uri.endsWith('/') ? 'index.html' : '';

        // Disallow serving anything outside out directory.
        const absPath = path.normalize(path.join(serveDir, uri));
        const relative = path.relative(serveDir, absPath);
        if (relative.startsWith('..')) {
          res.writeHead(404);
          res.end();
          return;
        }

        fs.readFile(absPath, function(err, data) {
          if (err) {
            res.writeHead(404);
            res.end(JSON.stringify(err));
            return;
          }
          const mimeMap = {
            'css': 'text/css',
            'png': 'image/png',
            'svg': 'image/svg+xml',
            'js': 'application/javascript',
          };
          const contentType = mimeMap[uri.split('.').pop()] || 'text/html';
          const head = {
            'Content-Type': contentType,
            'Content-Length': data.length,
            'Cache-Control': 'no-cache',
          };
          res.writeHead(200, head);
          res.end(data);
        });
      })
      .listen(port, 'localhost');
}

function watchDir(dir) {
  const absDir = path.isAbsolute(dir) ? dir : pjoin(ROOT_DIR, dir);
  // Add a fs watch if in watch mode.
  if (cfg.watch) {
    fs.watch(absDir, {recursive: true}, (_eventType, filePath) => {
      if (cfg.verbose) {
        console.log('File change detected', _eventType, filePath);
      }
      ninjaBuild();
    });
  }
}

function exec(cmd, args, opts) {
  opts = opts || {};
  opts.stdout = opts.stdout || 'inherit';
  if (cfg.verbose) console.log(`${cmd} ${args.join(' ')}\n`);
  const spwOpts = {cwd: cfg.outDir, stdio: ['ignore', opts.stdout, 'inherit']};
  const checkExitCode = (code, signal) => {
    if (signal === 'SIGINT' || signal === 'SIGTERM') return;
    if (code !== 0 && !opts.noErrCheck) {
      console.error(`${cmd} ${args.join(' ')} failed with code ${code}`);
      process.exit(1);
    }
  };
  const spawnRes = child_process.spawnSync(cmd, args, spwOpts);
  checkExitCode(spawnRes.status, spawnRes.signal);
  return spawnRes;
}

function ensureDir(dirPath, clean) {
  const exists = fs.existsSync(dirPath);
  if (exists && clean) {
    if (cfg.verbose) console.log('rm', dirPath);
    fs.rmSync(dirPath, {recursive: true});
  }
  if (!exists || clean) fs.mkdirSync(dirPath, {recursive: true});
  return dirPath;
}

main();
