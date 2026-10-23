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

// Serves demo_backend.ts over HTTP with the UI on the same origin, as a live
// server does: a built UI (--ui-dist) or the dev server's (--ui-proxy). See
// README.md.
//
//   /                 ─► redirect to the UI page of ?target=<id>
//   /api/*, /demo/*   ─► DemoBackend
//   anything else     ─► --ui-dist files, or --ui-proxy (WebSockets too)
//
// Requests must name a loopback host or --host, against DNS rebinding. Other
// sites may only navigate here, and not to /demo/*.

// No "node:" prefix: tools/check_imports rejects it.
import * as fs from 'fs';
import * as http from 'http';
import type * as net from 'net';
import * as path from 'path';
import * as stream from 'stream';
import {parseArgs} from 'util';
import type {LiveHudPlacement} from '../controller';
import type * as Backend from './demo_backend';

// Node runs this file without a bundler, so the import needs the .ts
// extension, which the UI's TypeScript config rejects in static imports.
const {DemoBackend, SECURITY_HEADERS} = (await import(
  new URL('demo_backend.ts', import.meta.url).href
)) as typeof Backend;

const USAGE = `Usage: ui/node ${path.relative(process.cwd(), process.argv[1]!)} [flags]

Serves a simulated live stream for com.google.LiveStream, with the UI.

  --port <n>          Port (default 10081).
  --host <address>    Address to listen on, and the host name allowed besides
                      loopback ones (default 127.0.0.1).
  --ui-dist <dir>     Serves a built UI, e.g. out/ui/ui/dist.
  --ui-proxy <url>    Forwards UI requests to ui/run-dev-server, e.g.
                      http://127.0.0.1:10000.
  --hud <placement>   Where the UI shows the HUD: panel, a side panel tab
                      (default), or top, a row above the topbar.

Settings, also changed by GET /demo/set?<name>=<value> (e.g. page_size=10):
  --seed <n>          Seed of the simulation (default 1).
  --rate <x>          Speed of the jobs, up to 10 (default 1).
  --lanes <n>         Worker tracks, up to 16 (default 4).
  --backfill <s>      History of new sessions, up to 600 (default 120).
  --history <s>       History kept (default 3600).
  --page-size <n>     Most entries per delta (default 20000).
  --tz <zone>         IANA time zone (default: this host's).
  --stall             Holds new data: the UI shows Buffering.
  --error <text>      Sets the stream's error: the UI shows Error.
  --seed-status <n>   Fails api/seed with this HTTP status.

GET /demo/restart?target=<id> restarts streams; GET /demo/state shows them.
`;

const CONTENT_TYPES: Readonly<Record<string, string>> = {
  '.css': 'text/css',
  '.html': 'text/html; charset=utf-8',
  '.ico': 'image/x-icon',
  '.js': 'text/javascript',
  '.json': 'application/json',
  '.map': 'application/json',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.ttf': 'font/ttf',
  '.wasm': 'application/wasm',
  '.woff2': 'font/woff2',
};

const {
  'port': port,
  'host': host,
  'ui-dist': uiDist,
  'ui-proxy': uiProxy,
  'hud': hudFlag,
  'help': help,
  'stall': stall,
  ...settings
} = parseFlags();
if (help === true) {
  process.stdout.write(USAGE);
  process.exit(0);
}
if (!/^\d+$/.test(port) || Number(port) > 65535) fail(`bad --port ${port}`);
if (hudFlag !== 'panel' && hudFlag !== 'top') fail(`bad --hud ${hudFlag}`);
const hud: LiveHudPlacement = hudFlag;

const backend = new DemoBackend();
const entries = Object.entries(settings).map(
  ([name, value]): [string, string] => [name.replaceAll('-', '_'), `${value}`],
);
if (stall === true) entries.push(['stall', '1']);
const settingsError = backend.set(entries);
if (settingsError !== undefined) fail(settingsError);

const distDir = uiDist === undefined ? undefined : realDist(uiDist);
const upstream = uiProxy === undefined ? undefined : httpUrl(uiProxy);

// Host headers of this server, e.g. "localhost:10081"; set once listening.
const hosts = new Set<string>();

const server = http.createServer((req, res) => {
  route(req, res).catch((e) => {
    console.error(`${req.method} ${req.url}: ${e}`);
    if (!res.headersSent) sendError(res, 500, 'internal error');
    res.end();
  });
});
server.on('upgrade', proxyUpgrade);
server.on('error', (e) => fail(e.message));
server.listen(Number(port), host, () => {
  const address = server.address();
  const actualPort = typeof address === 'object' ? address?.port : undefined;
  // IPv6 addresses are in brackets in Host headers.
  const name = host.includes(':') ? `[${host}]` : host;
  for (const known of ['127.0.0.1', 'localhost', '[::1]', name]) {
    hosts.add(`${known}:${actualPort}`.toLowerCase());
    if (actualPort === 80) hosts.add(known.toLowerCase());
  }
  console.log(`Live stream demo on http://${name}:${actualPort}/`);
  console.log(`UI from ${distDir ?? upstream?.href ?? 'nowhere: API only'}`);
});
process.on('SIGINT', () => {
  backend.dispose();
  process.exit(0);
});

function parseFlags() {
  try {
    return parseArgs({
      options: {
        'port': {type: 'string', default: '10081'},
        'host': {type: 'string', default: '127.0.0.1'},
        'ui-dist': {type: 'string'},
        'ui-proxy': {type: 'string'},
        'hud': {type: 'string', default: 'panel'},
        'help': {type: 'boolean'},
        // Settings, as in demo/set.
        'seed': {type: 'string'},
        'rate': {type: 'string'},
        'lanes': {type: 'string'},
        'backfill': {type: 'string'},
        'history': {type: 'string'},
        'page-size': {type: 'string'},
        'tz': {type: 'string'},
        'stall': {type: 'boolean'},
        'error': {type: 'string'},
        'seed-status': {type: 'string'},
      },
    }).values;
  } catch (e) {
    return fail(e instanceof Error ? e.message : `${e}`);
  }
}

// The real path of --ui-dist: served files must be within it, also through
// links.
function realDist(dir: string): string {
  let real = '';
  try {
    real = fs.realpathSync(dir);
  } catch (e) {
    fail(`bad --ui-dist ${dir}: ${e instanceof Error ? e.message : e}`);
  }
  if (!fs.existsSync(path.join(real, 'index.html'))) {
    fail(`no index.html in --ui-dist ${real}`);
  }
  return real;
}

// The --ui-proxy URL: http only, as the dev server.
function httpUrl(value: string): URL {
  const url = URL.canParse(value) ? new URL(value) : undefined;
  if (url?.protocol !== 'http:') fail(`bad --ui-proxy ${value}`);
  return url;
}

async function route(req: http.IncomingMessage, res: http.ServerResponse) {
  if (!hosts.has((req.headers.host ?? '').toLowerCase())) {
    sendError(res, 403, 'unknown Host: use 127.0.0.1, localhost or --host');
    return;
  }
  if (req.method !== 'GET' && req.method !== 'HEAD') {
    res.setHeader('Allow', 'GET, HEAD');
    sendError(res, 405, 'only GET and HEAD are allowed');
    return;
  }
  const url = new URL(req.url ?? '/', 'http://localhost');

  // Other sites may only navigate here, e.g. no <img src="/api/stream">, and
  // not to the dev controls.
  const site = req.headers['sec-fetch-site'] ?? 'none';
  const navigation =
    req.headers['sec-fetch-mode'] === 'navigate' &&
    req.headers['sec-fetch-dest'] === 'document';
  const crossSite = site !== 'same-origin' && site !== 'none';
  if (crossSite && (!navigation || url.pathname.startsWith('/demo/'))) {
    sendError(
      res,
      403,
      'cross-site requests must be navigations outside /demo/',
    );
    return;
  }

  if (url.pathname === '/') {
    const page = backend.pagePath(url.searchParams.get('target') ?? '', hud);
    send(res, 302, {'Location': page, 'Cache-Control': 'no-store'});
    return;
  }

  // Ends the long-polls of clients that leave.
  const abort = new AbortController();
  res.on('close', () => abort.abort());
  const reply = backend.handle(url, abort.signal);
  if (reply !== undefined) {
    const {status, headers, body} = await reply;
    send(res, status, headers, body);
  } else if (upstream !== undefined) {
    proxy(req, res, upstream);
  } else if (distDir !== undefined) {
    await serveFile(res, distDir, url.pathname);
  } else {
    sendError(res, 404, 'no UI: start with --ui-dist or --ui-proxy');
  }
}

function send(
  res: http.ServerResponse,
  status: number,
  headers: Readonly<Record<string, string>>,
  body?: Uint8Array | string,
) {
  res.writeHead(status, {...headers, ...SECURITY_HEADERS});
  res.end(body);
}

function sendError(res: http.ServerResponse, status: number, error: string) {
  const headers = {'Content-Type': 'application/json'};
  send(res, status, headers, JSON.stringify({error}));
}

// Serves a file of the UI. The UI ignores the query of static files.
async function serveFile(
  res: http.ServerResponse,
  root: string,
  pathname: string,
) {
  let file: string;
  try {
    file = path.join(root, path.normalize(decodeURIComponent(pathname)));
  } catch {
    sendError(res, 400, 'bad path escaping');
    return;
  }
  // Opened before its stat: the size is of the file sent, also while a
  // rebuild replaces files.
  const real = await fs.promises.realpath(file).catch(() => '');
  const handle = real.startsWith(root + path.sep)
    ? await fs.promises.open(real).catch(() => undefined)
    : undefined;
  const stat = await handle?.stat().catch(() => undefined);
  if (handle === undefined || stat?.isFile() !== true) {
    await handle?.close();
    sendError(res, 404, `not found: ${pathname}`);
    return;
  }
  const type = CONTENT_TYPES[path.extname(real)] ?? 'application/octet-stream';
  res.writeHead(200, {
    'Content-Type': type,
    'Content-Length': String(stat.size),
    'Cache-Control': 'no-cache',
    ...SECURITY_HEADERS,
  });
  if (res.req.method === 'HEAD') {
    await handle.close();
    res.end();
    return;
  }
  // Read errors end the response early. The stream closes the file.
  stream.pipeline(handle.createReadStream(), res, (e) => {
    if (e) res.destroy();
  });
}

// Forwards a UI request to the dev server, with this server's headers.
function proxy(
  req: http.IncomingMessage,
  res: http.ServerResponse,
  target: URL,
) {
  const out = http.request(
    {
      host: target.hostname,
      port: target.port,
      method: req.method,
      path: req.url,
      headers: {...req.headers, host: target.host},
    },
    (up) => {
      const headers = {...up.headers};
      for (const name of Object.keys(SECURITY_HEADERS)) {
        delete headers[name.toLowerCase()];
      }
      res.writeHead(up.statusCode ?? 502, {...headers, ...SECURITY_HEADERS});
      stream.pipeline(up, res, (e) => {
        if (e) res.destroy();
      });
    },
  );
  out.on('error', (e) => {
    if (res.headersSent) {
      res.destroy();
    } else {
      sendError(res, 502, `dev server ${target.host}: ${e.message}`);
    }
  });
  stream.pipeline(req, out, (e) => {
    if (e) out.destroy();
  });
}

// Forwards WebSocket upgrades to the dev server, e.g. for live reload.
function proxyUpgrade(
  req: http.IncomingMessage,
  socket: net.Socket,
  head: Buffer,
) {
  socket.on('error', () => socket.destroy());
  if (!hosts.has((req.headers.host ?? '').toLowerCase())) {
    socket.end('HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n');
    return;
  }
  if (upstream === undefined) {
    socket.destroy();
    return;
  }
  const out = http.request({
    host: upstream.hostname,
    port: upstream.port,
    method: req.method,
    path: req.url,
    headers: {...req.headers, host: upstream.host},
  });
  out.on('upgrade', (up, upSocket, upHead) => {
    upSocket.on('error', () => socket.destroy());
    socket.on('close', () => upSocket.destroy());
    const lines = [`HTTP/1.1 101 ${up.statusMessage ?? 'Switching Protocols'}`];
    for (let i = 0; i < up.rawHeaders.length; i += 2) {
      lines.push(`${up.rawHeaders[i]}: ${up.rawHeaders[i + 1]}`);
    }
    socket.write(`${lines.join('\r\n')}\r\n\r\n`);
    socket.write(upHead);
    upSocket.write(head);
    upSocket.pipe(socket).pipe(upSocket);
  });
  out.on('response', (up) => {
    socket.end(`HTTP/1.1 ${up.statusCode} ${up.statusMessage}\r\n\r\n`);
  });
  out.on('error', () => socket.destroy());
  out.end();
}

function fail(message: string): never {
  console.error(`demo_server: ${message}\n\n${USAGE}`);
  process.exit(1);
}
