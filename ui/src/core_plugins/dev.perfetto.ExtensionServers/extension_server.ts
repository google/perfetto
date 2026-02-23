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
import {fetchWithTimeout} from '../../base/http_utils';
import {
  ExtensionServer,
  macrosSchema,
  Manifest,
  manifestSchema,
  protoDescriptorsSchema,
  UserInput,
  sqlModulesSchema,
} from './types';
import m from 'mithril';
import {showModal} from '../../widgets/modal';
import {Anchor} from '../../widgets/anchor';
import {CodeSnippet} from '../../widgets/code_snippet';
import {errResult, okResult, Result} from '../../base/result';
import {AppImpl} from '../../core/app_impl';
import {base64Encode, utf8Encode} from '../../base/string_utils';
import {joinPath} from './url_utils';

// =============================================================================
// Helpers
// =============================================================================

const FETCH_TIMEOUT_MS = 10000; // 10 seconds

interface FetchRequest {
  url: string;
  init: RequestInit;
}

// Builds the final fetch URL and RequestInit from a server location and a
// resource path. All URL construction and auth-specific header/credential
// additions happen here. Exported for testing.
export function buildFetchRequest(
  server: UserInput,
  path: string,
): FetchRequest {
  if (server.type === 'github') {
    const fullPath = joinPath(server.path, path);
    if (server.auth.type === 'github_pat') {
      // Use the GitHub API for authenticated requests (supports private repos).
      const url =
        `https://api.github.com/repos/${server.repo}` +
        `/contents/${fullPath.split('/').map(encodeURIComponent).join('/')}` +
        `?ref=${encodeURIComponent(server.ref)}`;
      const headers: Record<string, string> = {
        Accept: 'application/vnd.github.raw+json',
        Authorization: `token ${server.auth.pat}`,
      };
      return {url, init: {method: 'GET', headers}};
    }
    // Use raw.githubusercontent.com for unauthenticated requests to avoid
    // GitHub API rate limits (403 errors).
    const encodedPath = fullPath.split('/').map(encodeURIComponent).join('/');
    const url =
      `https://raw.githubusercontent.com/${server.repo}` +
      `/${encodeURIComponent(server.ref)}/${encodedPath}`;
    return {url, init: {method: 'GET'}};
  }

  // HTTPS servers — normalize URL in case https:// is missing.
  let baseUrl = server.url.trim();
  if (!baseUrl.includes('://')) {
    baseUrl = `https://${baseUrl}`;
  }
  const url = `${baseUrl.replace(/\/+$/, '')}/${path}`;
  const headers: Record<string, string> = {};
  if (server.auth.type === 'https_basic') {
    const credentials = `${server.auth.username}:${server.auth.password}`;
    headers['Authorization'] = `Basic ${base64Encode(utf8Encode(credentials))}`;
  } else if (server.auth.type === 'https_apikey') {
    const {keyType, key} = server.auth;
    if (keyType === 'bearer') {
      headers['Authorization'] = `Bearer ${key}`;
    } else if (keyType === 'x_api_key') {
      headers['X-API-Key'] = key;
    } else {
      headers[server.auth.customHeaderName] = key;
    }
  } else if (server.auth.type === 'https_sso') {
    return {url, init: {method: 'GET', headers, credentials: 'include'}};
  }
  return {url, init: {method: 'GET', headers}};
}

const SSO_IFRAME_TIMEOUT_MS = 10000; // 10 seconds

// Refreshes SSO cookies by loading the server's base URL in a hidden iframe.
// The iframe follows any SSO redirects; once the onload fires, the browser
// should have fresh session cookies. Returns true on success, false on error
// or timeout.
function refreshSsoCookie(url: string): Promise<boolean> {
  return new Promise<boolean>((resolve) => {
    const iframe = document.createElement('iframe');
    iframe.style.display = 'none';
    iframe.src = url;
    const done = (ok: boolean) => {
      iframe.remove();
      resolve(ok);
    };
    const timer = setTimeout(() => done(false), SSO_IFRAME_TIMEOUT_MS);
    iframe.onload = () => {
      clearTimeout(timer);
      done(true);
    };
    iframe.onerror = () => {
      clearTimeout(timer);
      done(false);
    };
    document.body.appendChild(iframe);
  });
}

async function fetchJson<T extends z.ZodTypeAny>(
  server: UserInput,
  path: string,
  schema: T,
): Promise<Result<z.infer<T>>> {
  const req = buildFetchRequest(server, path);
  let response: Response;
  try {
    response = await fetchWithTimeout(req.url, req.init, FETCH_TIMEOUT_MS);
  } catch (e) {
    return errResult(`Failed to fetch ${req.url}: ${e}`);
  }

  // For SSO auth, a 403 may mean the cookie expired. Try refreshing via
  // an iframe and retry exactly once.
  if (
    response.status === 403 &&
    server.type === 'https' &&
    server.auth.type === 'https_sso'
  ) {
    let baseUrl = server.url.trim();
    if (!baseUrl.includes('://')) {
      baseUrl = `https://${baseUrl}`;
    }
    const refreshed = await refreshSsoCookie(baseUrl.replace(/\/+$/, ''));
    if (refreshed) {
      try {
        response = await fetchWithTimeout(req.url, req.init, FETCH_TIMEOUT_MS);
      } catch (e) {
        return errResult(`Failed to fetch ${req.url} after SSO refresh: ${e}`);
      }
    }
  }

  if (!response.ok) {
    return errResult(`Fetch failed: ${req.url} returned ${response.status}`);
  }
  let json: unknown;
  try {
    json = await response.json();
  } catch (e) {
    return errResult(`Failed to parse JSON from ${req.url}: ${e}`);
  }
  const result = schema.safeParse(json);
  if (!result.success) {
    return errResult(
      `Invalid response from ${req.url}: ${result.error.message}`,
    );
  }
  return okResult(result.data);
}

// Returns the human-readable source label for a module. Used as the command
// palette chip text. Format: "Server Name" for default, "Server: Module" for
// non-default modules.
function sourceLabel(
  manifest: Result<Manifest>,
  modId: string,
): string | undefined {
  if (!manifest.ok) return undefined;
  if (modId === 'default') return manifest.value.name;
  const entry = manifest.value.modules.find((m) => m.id === modId);
  return `${manifest.value.name}: ${entry?.name ?? modId}`;
}

// =============================================================================
// Loading
// =============================================================================

export async function loadManifest(
  server: UserInput,
): Promise<Result<Manifest>> {
  return fetchJson(server, 'manifest', manifestSchema);
}

function modulePath(module: string, manifest: Manifest): string | undefined {
  const entry = manifest.modules.find((m) => m.id === module);
  if (entry === undefined) return undefined;
  return `modules/${module}`;
}

async function loadMacros(
  manifestResult: Result<Manifest>,
  server: UserInput,
  module: string,
) {
  if (!manifestResult.ok) {
    return errResult(manifestResult.error);
  }
  const manifest = manifestResult.value;
  const modPath = modulePath(module, manifest);
  if (modPath === undefined) {
    return errResult(`Module '${module}' not found on server`);
  }
  // Check if macros are supported.
  if (!manifest.features.find((f) => f.name === 'macros')) {
    // Not supported, return empty list.
    return okResult([]);
  }
  const wrapper = await fetchJson(server, `${modPath}/macros`, macrosSchema);
  if (!wrapper.ok) {
    return errResult(wrapper.error);
  }
  // Validate that all macro IDs start with the namespace
  for (const macro of wrapper.value.macros) {
    // TODO(lalitm): remove this once Google3 is properly migrated.
    const isLegacy = macro.id.startsWith('dev.perfetto.UserMacro.');
    if (!macro.id.startsWith(manifest.namespace + '.') && !isLegacy) {
      return errResult(
        `Macro ID '${macro.id}' must start with namespace '${manifest.namespace}.'`,
      );
    }
  }
  return okResult(wrapper.value.macros);
}

async function loadSqlPackage(
  manifestResult: Result<Manifest>,
  server: UserInput,
  module: string,
) {
  if (!manifestResult.ok) {
    return errResult(manifestResult.error);
  }
  const manifest = manifestResult.value;
  const modPath = modulePath(module, manifest);
  if (modPath === undefined) {
    return errResult(`Module '${module}' not found on server`);
  }
  // Check if sql_modules are supported.
  if (!manifest.features.find((f) => f.name === 'sql_modules')) {
    // Not supported, return empty list.
    return okResult([]);
  }
  const wrapper = await fetchJson(
    server,
    `${modPath}/sql_modules`,
    sqlModulesSchema,
  );
  if (!wrapper.ok) {
    return errResult(wrapper.error);
  }
  for (const sqlModule of wrapper.value.sql_modules) {
    if (!sqlModule.name.startsWith(manifest.namespace + '.')) {
      return errResult(
        `SQL module name '${sqlModule.name}' must start with namespace '${manifest.namespace}.'`,
      );
    }
  }
  return okResult([
    {
      name: manifest.namespace,
      modules: wrapper.value.sql_modules,
    },
  ]);
}

async function loadProtoDescriptors(
  manifestResult: Result<Manifest>,
  server: UserInput,
  module: string,
) {
  if (!manifestResult.ok) {
    return errResult(manifestResult.error);
  }
  const manifest = manifestResult.value;
  const modPath = modulePath(module, manifest);
  if (modPath === undefined) {
    return errResult(`Module '${module}' not found on server`);
  }
  // Check if proto_descriptors are supported.
  if (!manifest.features.find((f) => f.name === 'proto_descriptors')) {
    // Not supported, return empty list.
    return okResult([]);
  }
  const wrapper = await fetchJson(
    server,
    `${modPath}/proto_descriptors`,
    protoDescriptorsSchema,
  );
  if (!wrapper.ok) {
    return errResult(wrapper.error);
  }
  return okResult(wrapper.value.proto_descriptors);
}

// =============================================================================
// Initialization
// =============================================================================

// Loads a single server's enabled modules given an already in-flight manifest
// promise. Returns the per-module result promises (for error aggregation).
export function initializeServerFromManifest(
  ctx: AppImpl,
  server: ExtensionServer,
  manifest: Promise<Result<Manifest>>,
): Promise<Result<unknown>[]> {
  const results: Promise<Result<unknown>>[] = [];
  for (const mod of server.enabledModules) {
    const macros = manifest.then((r) => loadMacros(r, server, mod));
    const sqlPackage = manifest.then((r) => loadSqlPackage(r, server, mod));
    const descs = manifest.then((r) => loadProtoDescriptors(r, server, mod));
    results.push(macros, sqlPackage, descs);
    ctx.addMacros(
      manifest.then(async (r) => {
        const macrosResult = await macros;
        if (!macrosResult.ok) return [];
        const source = sourceLabel(r, mod);
        return macrosResult.value.map((m) => ({...m, source}));
      }),
    );
    ctx.addSqlPackages(sqlPackage.then((r) => (r.ok ? r.value : [])));
    ctx.addProtoDescriptors(descs.then((r) => (r.ok ? r.value : [])));
  }
  return Promise.all(results);
}

// Initializes extension servers by fetching manifests and loading extensions
// (asynchronously). Returns the result promises so the caller can aggregate
// errors (e.g. with embedder server results) before showing a single modal.
export async function initializeServers(
  ctx: AppImpl,
  servers: ReadonlyArray<ExtensionServer>,
): Promise<Result<unknown>[]> {
  const results: Promise<Result<unknown>[]>[] = [];
  for (const server of servers) {
    if (!server.enabled) {
      continue;
    }
    results.push(
      initializeServerFromManifest(ctx, server, loadManifest(server)),
    );
  }
  const perServerResults = await Promise.all(results);
  return perServerResults.flat();
}

// When all the extension loading promises complete, show a modal if there
// were any errors. Deduplicate errors since a manifest fetch failure
// propagates to all downstream loaders (macros, sql_modules, etc.).
export function showErrorsOnCompletion(results: Result<unknown>[]): void {
  const uniqueErrors = [
    ...new Set(results.filter((r) => !r.ok).map((r) => r.error)),
  ];
  if (uniqueErrors.length > 0) {
    const n = uniqueErrors.length;
    showModal({
      title: `${n} error${n === 1 ? '' : 's'} while querying extension servers`,
      content: m(
        'div',
        m(CodeSnippet, {
          text: uniqueErrors.map((e) => `• ${e}`).join('\n'),
          class: 'pf-ext-server-errors',
        }),
        m('p', [
          'For more information see the ',
          m(
            Anchor,
            {
              href: 'https://perfetto.dev/docs/visualization/extensions',
              target: '_blank',
            },
            'extension servers documentation',
          ),
          '.',
        ]),
      ),
      buttons: [{text: 'OK', primary: true}],
    });
  }
}
