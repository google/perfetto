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

// Whether the servers of the picker answer, for its status dots and for the
// local server setup, which waits for the server to come up.

import m from 'mithril';
import {
  BigtraceHttpError,
  BigtraceQueryClient,
  QueryNotFoundError,
  type ServerInfo,
} from '../query/bigtrace_query_client';
import {isLoopbackEndpoint, normalizeEndpoint} from './server_list';

export type ProbeResult =
  | {readonly state: 'checking'}
  | {readonly state: 'online'; readonly info?: ServerInfo}
  | {readonly state: 'offline'};

const PROBE_TIMEOUT_MS = 4000;

// Asks `endpoint` whether it is up. `GET /` answers with the local server's
// info; a backend without that route (the Bigtrace service) answers with an
// HTTP error, or with no CORS headers, which looks like a network error, so
// for non-loopback servers a static route is tried too.
export async function probeServer(
  endpoint: string,
  timeoutMs = PROBE_TIMEOUT_MS,
): Promise<ProbeResult> {
  const client = new BigtraceQueryClient(normalizeEndpoint(endpoint));
  const answered = (e: unknown) =>
    e instanceof BigtraceHttpError || e instanceof QueryNotFoundError;
  try {
    const info = await client.getServerInfo(AbortSignal.timeout(timeoutMs));
    return {state: 'online', info: info.service ? info : undefined};
  } catch (e) {
    if (answered(e)) return {state: 'online'};
    if (isLoopbackEndpoint(endpoint)) return {state: 'offline'};
  }
  try {
    await client.listTracePresets(AbortSignal.timeout(timeoutMs));
    return {state: 'online'};
  } catch (e) {
    return answered(e) ? {state: 'online'} : {state: 'offline'};
  }
}

// The latest probe of each endpoint; views read it and redraw on changes.
class ServerProbes {
  private results = new Map<string, ProbeResult>();
  private inFlight = new Set<string>();

  get(endpoint: string): ProbeResult | undefined {
    return this.results.get(normalizeEndpoint(endpoint));
  }

  // Probes `endpoint` unless a probe of it is already running; resolves with
  // the result.
  async refresh(endpoint: string): Promise<ProbeResult | undefined> {
    const key = normalizeEndpoint(endpoint);
    if (this.inFlight.has(key)) return this.results.get(key);
    this.inFlight.add(key);
    if (!this.results.has(key)) this.results.set(key, {state: 'checking'});
    try {
      const result = await probeServer(key);
      this.results.set(key, result);
      return result;
    } finally {
      this.inFlight.delete(key);
      m.redraw();
    }
  }
}

export const serverProbes = new ServerProbes();
