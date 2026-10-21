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

// The Bigtrace servers the user can pick from: the Bigtrace service, the
// local server (tools/bigtrace_server.py, usually the always-on `bigtrace
// install` service, possibly reached over `bigtrace tunnel`), and any number
// of servers the user added (e.g. a colleague's). The picked one is the
// `bigtraceEndpoint` setting; the added ones are the `bigtraceServers` one.

import {z} from 'zod';
import {
  DEFAULT_ENDPOINT,
  endpointStorage,
  getBigtraceEndpoint,
} from './endpoint_storage';

export const LOCAL_PORT = 5001;
export const LOCAL_ENDPOINT = `http://127.0.0.1:${LOCAL_PORT}/v1`;
const LOOPBACK_HOSTS = ['localhost', '127.0.0.1', '[::1]'];

export type ServerKind = 'service' | 'local' | 'custom';

export interface BigtraceServer {
  readonly name: string;
  readonly endpoint: string;
  readonly kind: ServerKind;
}

const savedServerSchema = z.array(
  z.object({name: z.string(), endpoint: z.string()}),
);

const savedServers = endpointStorage.register({
  id: 'bigtraceServers',
  name: 'BigTrace servers',
  description: 'Servers added to the server picker.',
  schema: savedServerSchema,
  defaultValue: [],
});

export function isLoopbackEndpoint(endpoint: string): boolean {
  const u = parseUrl(endpoint);
  return u !== undefined && LOOPBACK_HOSTS.includes(u.hostname);
}

// Endpoints are compared without a trailing slash.
export function sameEndpoint(a: string, b: string): boolean {
  return a.trim().replace(/\/+$/, '') === b.trim().replace(/\/+$/, '');
}

// The built-in servers, then the added ones, without duplicates.
export function listServers(): BigtraceServer[] {
  const servers: BigtraceServer[] = [
    {
      name: globalThis.__BIGTRACE_DEV_PROXY__
        ? 'Dev server backend'
        : 'BigTrace service',
      endpoint: DEFAULT_ENDPOINT,
      kind: 'service',
    },
    {name: 'Local server', endpoint: LOCAL_ENDPOINT, kind: 'local'},
  ];
  for (const s of savedServers.get()) {
    if (!servers.some((x) => sameEndpoint(x.endpoint, s.endpoint))) {
      servers.push({...s, kind: 'custom'});
    }
  }
  return servers;
}

export function activeServer(): BigtraceServer | undefined {
  const endpoint = getBigtraceEndpoint();
  return listServers().find((s) => sameEndpoint(s.endpoint, endpoint));
}

// Why `endpoint` cannot be a server for a page at `pageUrl`, or undefined if
// it can.
export function endpointProblem(
  endpoint: string,
  pageUrl: string = window.location.href,
): string | undefined {
  const u = parseUrl(endpoint.trim());
  if (u === undefined || (u.protocol !== 'http:' && u.protocol !== 'https:')) {
    return 'Enter an http:// or https:// URL, e.g. http://127.0.0.1:5001/v1';
  }
  if (u.search !== '' || u.hash !== '' || u.username !== '') {
    return 'The URL must not have a query, fragment or user name.';
  }
  // Browsers block http:// requests from https:// pages ("mixed content"),
  // except to loopback addresses.
  if (
    u.protocol === 'http:' &&
    new URL(pageUrl).protocol === 'https:' &&
    !LOOPBACK_HOSTS.includes(u.hostname)
  ) {
    return (
      'This page can only reach http:// servers on 127.0.0.1 or localhost. ' +
      'For a server on another machine, run `bigtrace tunnel HOST` here and ' +
      'use the local server, or serve it over https://.'
    );
  }
  return undefined;
}

export function normalizeEndpoint(endpoint: string): string {
  return endpoint.trim().replace(/\/+$/, '');
}

// Why a server at `endpoint` cannot be added to the picker (or replace the
// added server at `replacing`), or undefined if it can.
export function serverProblem(
  endpoint: string,
  replacing?: string,
): string | undefined {
  const problem = endpointProblem(endpoint);
  if (problem !== undefined) return problem;
  const existing = listServers().find(
    (s) =>
      sameEndpoint(s.endpoint, endpoint) &&
      (replacing === undefined || !sameEndpoint(s.endpoint, replacing)),
  );
  return existing === undefined
    ? undefined
    : `Already in the list as "${existing.name}".`;
}

function savedServer(name: string, endpoint: string) {
  const normalized = normalizeEndpoint(endpoint);
  return {name: name.trim() || new URL(normalized).host, endpoint: normalized};
}

// Adds a server to the picker; throws with the reason if it can't be added.
export function addServer(name: string, endpoint: string): BigtraceServer {
  const problem = serverProblem(endpoint);
  if (problem !== undefined) throw new Error(problem);
  const server = savedServer(name, endpoint);
  savedServers.set([...savedServers.get(), server]);
  return {...server, kind: 'custom'};
}

// Renames and/or re-points the added server at `oldEndpoint`; throws with the
// reason if it can't. Returns whether it was the active server, which then
// follows it (the caller reloads).
export function updateServer(
  oldEndpoint: string,
  name: string,
  endpoint: string,
): boolean {
  const problem = serverProblem(endpoint, oldEndpoint);
  if (problem !== undefined) throw new Error(problem);
  const server = savedServer(name, endpoint);
  savedServers.set(
    savedServers
      .get()
      .map((s) => (sameEndpoint(s.endpoint, oldEndpoint) ? server : s)),
  );
  const wasActive = sameEndpoint(getBigtraceEndpoint(), oldEndpoint);
  if (wasActive && !sameEndpoint(oldEndpoint, server.endpoint)) {
    selectServer(server.endpoint);
    return true;
  }
  return false;
}

export function removeServer(endpoint: string): void {
  savedServers.set(
    savedServers.get().filter((s) => !sameEndpoint(s.endpoint, endpoint)),
  );
}

// Makes `endpoint` the server the app talks to. The endpoint is read when
// modules initialize, so the caller reloads the page afterwards.
export function selectServer(endpoint: string): void {
  endpointStorage
    .get<string>('bigtraceEndpoint')
    ?.set(normalizeEndpoint(endpoint));
}

// Whether the app talks to the default server (the Bigtrace service).
export function isDefaultServerActive(): boolean {
  return sameEndpoint(getBigtraceEndpoint(), DEFAULT_ENDPOINT);
}

// Goes back to the default server, as the endpoint setting's reset did. The
// added servers stay. The caller reloads the page afterwards.
export function resetServer(): void {
  endpointStorage.get<string>('bigtraceEndpoint')?.reset();
}

function parseUrl(s: string): URL | undefined {
  try {
    return new URL(s);
  } catch {
    return undefined;
  }
}
