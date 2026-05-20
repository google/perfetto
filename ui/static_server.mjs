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

// Simple static file server used by `ui/build.mjs --serve` (without --watch).
// Serves files out of |distRootDir|, with a special branch that maps /test/*
// to <rootDir>/test/* so e2e tests can reach files under //test/ without
// symlinking them into dist/ (which ships to production).

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const MIME_MAP = {
  html: 'text/html',
  css: 'text/css',
  js: 'application/javascript',
  wasm: 'application/wasm',
};

// Starts the static server. |opts|:
//   rootDir              repo root (used for the /test/ branch and as the
//                        outer sandbox for path traversal checks).
//   distRootDir          directory served at /.
//   host, port           bind address; if port is undefined, defaults to
//                        |defaultPort| and auto-bumps on EADDRINUSE.
//   defaultPort          fallback when port is undefined.
//   portWasExplicit      if true, EADDRINUSE is fatal instead of retried.
//   crossOriginIsolation if true, adds COOP/COEP headers.
export function startStaticServer(opts) {
  const {
    rootDir,
    distRootDir,
    host,
    defaultPort,
    portWasExplicit,
    crossOriginIsolation,
  } = opts;

  const server = http.createServer((req, res) => {
    console.debug(req.method, req.url);
    let uri = req.url.split('?', 1)[0];
    if (uri.endsWith('/')) uri += 'index.html';

    let absPath = path.normalize(path.join(distRootDir, uri));
    // We want to be able to use the data in '/test/' for e2e tests. However,
    // we don't want to create a symlink into the 'dist/' dir, because 'dist/'
    // gets shipped on the production server.
    if (uri.startsWith('/test/')) {
      absPath = path.join(rootDir, uri);
    }

    // Don't serve contents outside of the project root (b/221101533).
    if (path.relative(rootDir, absPath).startsWith('..')) {
      res.writeHead(403);
      res.end('403 Forbidden - Request path outside of the repo root');
      return;
    }

    let stat;
    try {
      stat = fs.statSync(absPath);
    } catch (statErr) {
      res.writeHead(404);
      res.end(JSON.stringify(statErr));
      return;
    }

    // Truncate to second precision: HTTP dates have 1s resolution, so the
    // sub-millisecond part of mtime would cause a permanent mismatch.
    const mtimeSec = Math.floor(stat.mtime.getTime() / 1000) * 1000;
    const mtimeStr = new Date(mtimeSec).toUTCString();

    const ifModifiedSince = req.headers['if-modified-since'];
    if (
      ifModifiedSince !== undefined &&
      new Date(ifModifiedSince).getTime() >= mtimeSec
    ) {
      res.writeHead(304);
      res.end();
      return;
    }

    fs.readFile(absPath, (err, data) => {
      if (err) {
        res.writeHead(404);
        res.end(JSON.stringify(err));
        return;
      }
      const ext = uri.split('.').pop();
      const cType = MIME_MAP[ext] || 'octect/stream';
      const acceptsGzip = (req.headers['accept-encoding'] || '').includes(
        'gzip',
      );
      const finalize = (body) => {
        const head = {
          'Content-Type': cType,
          'Content-Length': body.length,
          'Last-Modified': mtimeStr,
          'Cache-Control': 'no-cache',
        };
        if (acceptsGzip) head['Content-Encoding'] = 'gzip';
        if (crossOriginIsolation) {
          head['Cross-Origin-Opener-Policy'] = 'same-origin';
          head['Cross-Origin-Embedder-Policy'] = 'require-corp';
        }
        res.writeHead(200, head);
        res.write(body);
        res.end();
      };
      if (acceptsGzip) {
        zlib.gzip(data, (gzErr, compressed) => {
          finalize(gzErr ? data : compressed);
        });
      } else {
        finalize(data);
      }
    });
  });

  let port = opts.port ?? defaultPort;
  let retryCount = 0;

  server.on('error', (e) => {
    if (e.code === 'EADDRINUSE') {
      if (!portWasExplicit && retryCount <= 10) {
        console.log(`Port ${port} is in use, trying ${port + 1}...`);
        ++port;
        ++retryCount;
        server.listen(port, host);
      } else if (portWasExplicit) {
        console.error(
          `ERROR: Port ${port} is in use, and --serve-port was explicitly set. Exiting.`,
        );
        process.exit(1);
      } else {
        console.error(
          `ERROR: Port ${port} is in use, and no free port found after 10 tries. Exiting.`,
        );
        process.exit(1);
      }
    } else {
      console.error('HTTP SERVER ERROR:', e);
      process.exit(1);
    }
  });

  server.listen(port, host);

  server.on('listening', () => {
    const {address, port} = server.address();
    const hostStr = address === '127.0.0.1' ? 'localhost' : address;
    console.log(`HTTP server is listening on http://${hostStr}:${port}`);
  });

  return server;
}
