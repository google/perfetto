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

import {afterEach, beforeEach, describe, expect, test, vi} from 'vitest';
import {DEFAULT_ENDPOINT, getBigtraceEndpoint} from './endpoint_storage';
import {
  allowOriginCommand,
  installCommand,
  tunnelCommand,
} from './local_server_setup';
import {
  activeServer,
  addServer,
  endpointProblem,
  LOCAL_ENDPOINT,
  listServers,
  removeServer,
  sameEndpoint,
  selectServer,
} from './server_list';
import {probeServer} from './server_probe';

beforeEach(() => {
  localStorage.clear();
});

afterEach(() => vi.unstubAllGlobals());

describe('server list', () => {
  test('starts with the service and the local server', () => {
    expect(listServers()).toEqual([
      {name: 'BigTrace service', endpoint: DEFAULT_ENDPOINT, kind: 'service'},
      {name: 'Local server', endpoint: LOCAL_ENDPOINT, kind: 'local'},
    ]);
    expect(activeServer()?.kind).toBe('service');
  });

  test('adds, selects and removes servers', () => {
    const s = addServer(' Alice ', 'http://127.0.0.1:5002/v1/');
    expect(s).toEqual({
      name: 'Alice',
      endpoint: 'http://127.0.0.1:5002/v1',
      kind: 'custom',
    });
    expect(listServers().map((x) => x.name)).toEqual([
      'BigTrace service',
      'Local server',
      'Alice',
    ]);
    selectServer('http://127.0.0.1:5002/v1/');
    expect(getBigtraceEndpoint()).toBe('http://127.0.0.1:5002/v1');
    expect(activeServer()?.name).toBe('Alice');
    removeServer('http://127.0.0.1:5002/v1');
    expect(listServers()).toHaveLength(2);
    expect(activeServer()).toBeUndefined();
  });

  test('names a server by its host when no name is given', () => {
    expect(addServer('', 'https://bt.example.com/v1').name).toBe(
      'bt.example.com',
    );
  });

  test('refuses duplicates and bad endpoints', () => {
    expect(() => addServer('x', `${LOCAL_ENDPOINT}/`)).toThrow(
      'Already in the list as "Local server"',
    );
    expect(() => addServer('x', 'not a url')).toThrow('http:// or https://');
  });

  test('flags endpoints a page cannot reach', () => {
    const https = 'https://bigtrace.example.com/';
    expect(endpointProblem('http://127.0.0.1:5001/v1', https)).toBeUndefined();
    expect(endpointProblem('http://localhost:7/v1', https)).toBeUndefined();
    expect(endpointProblem('https://other.example/v1', https)).toBeUndefined();
    expect(endpointProblem('http://ws.example:5001/v1', https)).toContain(
      'bigtrace tunnel',
    );
    // From an http:// page (e.g. a dev server), any http:// server works.
    expect(
      endpointProblem('http://ws.example:5001/v1', 'http://localhost:10000/'),
    ).toBeUndefined();
    expect(endpointProblem('ftp://x.example', https)).toBeDefined();
    expect(endpointProblem('http://127.0.0.1:5001/v1?a=b', https)).toContain(
      'query',
    );
  });

  test('compares endpoints without trailing slashes', () => {
    expect(sameEndpoint('http://a/v1/', 'http://a/v1')).toBe(true);
    expect(sameEndpoint('http://a/v1', 'http://b/v1')).toBe(false);
  });
});

describe('setup commands', () => {
  test('allows the page origin when the server does not by default', () => {
    expect(installCommand('https://brush-ui.example.com')).toBe(
      'curl -fsSL https://raw.githubusercontent.com/google/perfetto/main/' +
        'tools/install-bigtrace | python3 - --cors-origin ' +
        'https://brush-ui.example.com',
    );
    for (const origin of [
      'https://ui.perfetto.dev',
      'http://localhost:10000',
      'http://127.0.0.1:10000',
    ]) {
      expect(installCommand(origin)).toMatch(/\| python3 -$/);
    }
  });

  test('forwards the local port', () => {
    expect(tunnelCommand('ws.example')).toBe(
      'ssh -N -L 5001:127.0.0.1:5001 ws.example',
    );
  });

  test('allows an origin on an installed server', () => {
    expect(allowOriginCommand('https://brush-ui.example.com')).toBe(
      'bigtrace install --cors-origin https://brush-ui.example.com',
    );
  });
});

describe('probeServer', () => {
  function stubFetch(handler: (url: string) => Promise<unknown>) {
    const fetchMock = vi.fn((url: string) => handler(url));
    vi.stubGlobal('fetch', fetchMock);
    return fetchMock;
  }
  const json = (body: unknown, status = 200) =>
    Promise.resolve({
      ok: status < 400,
      status,
      json: async () => body,
      text: async () => JSON.stringify(body),
    });

  test('reports the local server and its traces', async () => {
    stubFetch(() =>
      json({service: 'bigtrace', traceCount: 3, traceDir: '/traces'}),
    );
    expect(await probeServer(LOCAL_ENDPOINT)).toEqual({
      state: 'online',
      info: {service: 'bigtrace', traceCount: 3, traceDir: '/traces'},
    });
  });

  test('a local server that does not answer is offline', async () => {
    const fetchMock = stubFetch(() =>
      Promise.reject(new TypeError('Failed to fetch')),
    );
    expect(await probeServer(LOCAL_ENDPOINT)).toEqual({state: 'offline'});
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  test('an HTTP error is an answer', async () => {
    stubFetch(() => json({detail: 'nope'}, 404));
    expect(await probeServer('https://bt.example.com/v1')).toEqual({
      state: 'online',
    });
  });

  test('falls back to a static route for remote servers', async () => {
    // The Bigtrace service has no `GET /`, and its 404 has no CORS headers.
    const fetchMock = stubFetch((url) =>
      url.endsWith('/trace_presets')
        ? json({tracePresets: []})
        : Promise.reject(new TypeError('Failed to fetch')),
    );
    expect(await probeServer('https://bt.example.com/v1')).toEqual({
      state: 'online',
    });
    expect(fetchMock.mock.calls.map((c) => c[0])).toEqual([
      'https://bt.example.com/v1/',
      'https://bt.example.com/v1/trace_presets',
    ]);
  });

  test('a remote server that answers nothing is offline', async () => {
    stubFetch(() => Promise.reject(new TypeError('Failed to fetch')));
    expect(await probeServer('https://bt.example.com/v1')).toEqual({
      state: 'offline',
    });
  });
});
