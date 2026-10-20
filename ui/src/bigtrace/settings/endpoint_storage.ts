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

import {z} from 'zod';
import {LocalStorage} from '../../core/local_storage';
import {
  LocalSettingsStorage,
  BIGTRACE_SETTINGS_STORAGE_KEY,
} from './settings_storage';

export const endpointStorage = new LocalSettingsStorage(
  new LocalStorage(BIGTRACE_SETTINGS_STORAGE_KEY),
);

// `?server=<url>` (as printed by tools/bigtrace_server.py) selects the
// backend: stored before registration, so no reload is asked for, and dropped
// from the address bar. The backend sees every query, so hosts other than
// loopback and the page's own origin need the user's confirmation.
function adoptEndpointFromUrl(): void {
  const url = new URL(window.location.href);
  const endpoint = url.searchParams.get('server');
  if (endpoint === null) return;
  if (
    isAutoTrustedEndpoint(endpoint, window.location.origin) ||
    (parseHttpUrl(endpoint) !== undefined &&
      window.confirm(`Use the BigTrace backend ${endpoint}?`))
  ) {
    endpointStorage.setStoredValue('bigtraceEndpoint', endpoint);
  }
  url.searchParams.delete('server');
  window.history.replaceState(window.history.state, '', url.toString());
}

function parseHttpUrl(s: string): URL | undefined {
  try {
    const u = new URL(s);
    return u.protocol === 'http:' || u.protocol === 'https:' ? u : undefined;
  } catch {
    return undefined;
  }
}

export function isAutoTrustedEndpoint(
  endpoint: string,
  pageOrigin: string,
): boolean {
  const u = parseHttpUrl(endpoint);
  return (
    u !== undefined &&
    (u.origin === pageOrigin ||
      ['localhost', '127.0.0.1', '[::1]'].includes(u.hostname))
  );
}

adoptEndpointFromUrl();

// Set (via a Vite define) when the ui/build.mjs dev server proxies /v1/* to
// the local Bigtrace server.
declare global {
  var __BIGTRACE_DEV_PROXY__: boolean | undefined;
}

// The backend used until the user picks another: the Bigtrace service, or the
// server the ui/build.mjs dev server proxies.
export const DEFAULT_ENDPOINT = globalThis.__BIGTRACE_DEV_PROXY__
  ? `${window.location.origin}/v1`
  : 'https://brush-googleapis.corp.google.com/v1';

endpointStorage.register({
  id: 'bigtraceEndpoint',
  name: 'BigTrace Endpoint',
  description: 'The URL of the BigTrace backend service.',
  schema: z.string(),
  defaultValue: DEFAULT_ENDPOINT,
  requiresReload: true,
});

// The configured BigTrace endpoint URL, or '' if unset. Centralizes the
// null/empty handling call sites would otherwise re-inline.
export function getBigtraceEndpoint(): string {
  const setting = endpointStorage.get('bigtraceEndpoint');
  return setting ? ((setting.get() as string) ?? '') : '';
}
