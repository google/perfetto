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
import {showModal} from '../../widgets/modal';
import {errResult, okResult, Result} from '../../base/result';
import {AppImpl} from '../../core/app_impl';
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
    const url =
      `https://api.github.com/repos/${server.repo}` +
      `/contents/${fullPath.split('/').map(encodeURIComponent).join('/')}` +
      `?ref=${encodeURIComponent(server.ref)}`;
    const headers: Record<string, string> = {
      Accept: 'application/vnd.github.raw+json',
    };
    if (server.auth.type === 'github_pat') {
      headers['Authorization'] = `token ${server.auth.pat}`;
    }
    return {url, init: {method: 'GET', headers}};
  }

  // HTTPS servers — normalize URL in case https:// is missing.
  let baseUrl = server.url.trim();
  if (!baseUrl.includes('://')) {
    baseUrl = `https://${baseUrl}`;
  }
  const url = `${baseUrl.replace(/\/+$/, '')}/${path}`;
  return {url, init: {method: 'GET'}};
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

// =============================================================================
// Loading
// =============================================================================

export async function loadManifest(
  server: UserInput,
): Promise<Result<Manifest>> {
  return fetchJson(server, 'manifest', manifestSchema);
}

function modulePath(module: string, manifest: Manifest): string | undefined {
  const entry = manifest.modules.find((m) => m.name === module);
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

// Initializes extension servers by fetching manifests (synchronously) and
// loading extensions (asynchronously). This function should be called early
// in app initialization.
export function initializeExtensions(
  ctx: AppImpl,
  servers: ReadonlyArray<ExtensionServer>,
) {
  const results = [];
  for (const server of servers) {
    if (!server.enabled) {
      continue;
    }
    const manifest = loadManifest(server);
    for (const mod of server.enabledModules) {
      const macros = manifest.then((r) => loadMacros(r, server, mod));
      const sqlPackage = manifest.then((r) => loadSqlPackage(r, server, mod));
      const descs = manifest.then((r) => loadProtoDescriptors(r, server, mod));
      results.push(macros, sqlPackage, descs);
      ctx.addMacros(macros.then((r) => (r.ok ? r.value : [])));
      ctx.addSqlPackages(sqlPackage.then((r) => (r.ok ? r.value : [])));
      ctx.addProtoDescriptors(descs.then((r) => (r.ok ? r.value : [])));
    }
  }
  // When all the extension loading promises complete, show a modal if there
  // were any errors.
  Promise.all(results).then((results) => {
    const errors = results
      .filter((r) => !r.ok)
      .map((r) => r.error)
      .join('\n');
    if (errors.length > 0) {
      showModal({
        title: 'Error(s) while querying extension servers',
        content: errors,
        buttons: [{text: 'OK', primary: true}],
      });
    }
  });
}
