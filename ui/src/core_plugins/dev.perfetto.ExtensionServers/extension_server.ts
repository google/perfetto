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
import {CommandInvocation} from '../../core/command_manager';
import {
  ExtensionServer,
  ExtensionServerState,
  macrosSchema,
  Manifest,
  manifestSchema,
  protoDescriptorsSchema,
  sqlModulesSchema,
} from './types';
import {normalizeServerKey, resolveServerUrl} from './url_utils';
import {showModal} from '../../widgets/modal';
import {errResult, okResult, Result} from '../../base/result';
import {AppImpl} from '../../core/app_impl';

const FETCH_TIMEOUT_MS = 5000; // 5 seconds

// =============================================================================
// Helpers
// =============================================================================

async function fetchJson<T extends z.ZodTypeAny>(
  url: string,
  schema: T,
): Promise<Result<z.infer<T>>> {
  let response: Response;
  try {
    response = await fetchWithTimeout(url, {method: 'GET'}, FETCH_TIMEOUT_MS);
  } catch (e) {
    return errResult(`Failed to fetch ${url}: ${e}`);
  }
  if (!response.ok) {
    return errResult(`Fetch failed: ${url} returned ${response.status}`);
  }
  let json: unknown;
  try {
    json = await response.json();
  } catch (e) {
    return errResult(`Failed to parse JSON from ${url}: ${e}`);
  }
  const result = schema.safeParse(json);
  if (!result.success) {
    return errResult(`Invalid response from ${url}: ${result.error.message}`);
  }
  return okResult(result.data);
}

function aggregate<T>(
  results: Array<ReadonlyArray<[string, T]>>,
  overwrite: boolean = true,
): Map<string, T> {
  const map = new Map<string, T>();
  for (const entries of results) {
    for (const [key, value] of entries) {
      if (overwrite || !map.has(key)) {
        map.set(key, value);
      }
    }
  }
  return map;
}

// =============================================================================
// Loading
// =============================================================================

export async function loadManifest(
  serverUrl: string,
): Promise<Result<Manifest>> {
  return fetchJson(serverUrl + '/manifest', manifestSchema);
}

async function loadModuleMacros(
  state: ExtensionServerState,
  module: string,
): Promise<Result<Array<[string, CommandInvocation[]]>>> {
  const wrapper = await fetchJson(
    `${state.canonicalUrl}/modules/${module}/macros`,
    macrosSchema,
  );
  if (!wrapper.ok) {
    return errResult(wrapper.error);
  }
  return okResult(
    Object.entries(wrapper.value.macros)
      .sort()
      .map(([name, commands]) => [
        `[${state.serverKey} ${module}] ${name}`,
        commands,
      ]),
  );
}

async function loadModuleSqlModules(
  state: ExtensionServerState,
  module: string,
): Promise<Result<Array<[string, string]>>> {
  const wrapper = await fetchJson(
    `${state.canonicalUrl}/modules/${module}/sql_modules`,
    sqlModulesSchema,
  );
  if (!wrapper.ok) {
    return errResult(wrapper.error);
  }
  return okResult(Object.entries(wrapper.value.modules).sort());
}

async function loadModuleProtoDescriptors(
  state: ExtensionServerState,
  module: string,
): Promise<Result<ReadonlyArray<string>>> {
  const wrapper = await fetchJson(
    `${state.canonicalUrl}/modules/${module}/proto_descriptors`,
    protoDescriptorsSchema,
  );
  if (!wrapper.ok) {
    return errResult(wrapper.error);
  }
  return okResult(wrapper.value.descriptors.sort());
}

// =============================================================================
// Runtime State Management
// =============================================================================

async function loadServerState(
  server: ExtensionServer,
): Promise<Result<ExtensionServerState>> {
  const canonicalUrl = resolveServerUrl(server.url);
  const serverKey = normalizeServerKey(canonicalUrl);
  const manifest = await loadManifest(canonicalUrl);
  if (!manifest.ok) {
    return errResult(manifest.error);
  }
  return okResult({
    url: server.url,
    enabledModules: server.enabledModules,
    enabled: server.enabled,
    canonicalUrl,
    serverKey,
    displayName: manifest.value.name,
    availableModules: manifest.value.modules,
  });
}

// =============================================================================
// Initialization
// =============================================================================

export interface ExtensionInitializationResult {
  states: ExtensionServerState[];
  macrosPromise: Promise<Map<string, ReadonlyArray<CommandInvocation>>>;
  sqlModulesPromise: Promise<Map<string, string>>;
  protoDescriptorsPromise: Promise<ReadonlyArray<string>>;
}

// Initializes extension servers by fetching manifests (synchronously) and
// loading extensions (asynchronously). This function should be called early
// in app initialization.
export async function initializeExtensions(
  ctx: AppImpl,
  servers: ExtensionServer[],
): Promise<void> {
  // Load server states (fetches manifests) - this is blocking
  const states = await Promise.all(servers.map(loadServerState));

  // Show errors
  const errors = states
    .filter((r) => !r.ok)
    .map((r) => r.error)
    .join('\n');
  if (errors.length > 0) {
    await showModal({
      title: 'Extension Servers Failed to Load',
      content: `Some extension servers failed to load:\n\n${errors}`,
      buttons: [{text: 'OK', primary: true}],
    });
  }

  // Sort alphabetically by serverKey for deterministic ordering
  const sorted = states
    .filter((r) => r.ok)
    .map((r) => r.value)
    .filter((s) => s.enabled)
    .sort((a, b) => a.serverKey.localeCompare(b.serverKey));

  // Fire off all loads in parallel
  const macroPromises = [];
  const sqlModulePromises = [];
  const protoDescriptorPromises = [];
  for (const state of sorted) {
    for (const module of [...state.enabledModules].sort()) {
      macroPromises.push(loadModuleMacros(state, module));
      sqlModulePromises.push(loadModuleSqlModules(state, module));
      protoDescriptorPromises.push(loadModuleProtoDescriptors(state, module));
    }
  }

  // Show errors once all loads are done.
  Promise.all([
    ...macroPromises,
    ...sqlModulePromises,
    ...protoDescriptorPromises,
  ]).then((results) => {
    const errors = results
      .filter((r) => !r.ok)
      .map((r) => r.error)
      .join('\n');
    if (errors.length > 0) {
      showModal({
        title: 'Extension Modules Failed to Load',
        content: `Some extension modules failed to load:\n\n${errors}`,
        buttons: [{text: 'OK', primary: true}],
      });
    }
  });

  ctx.addMacros(
    Promise.all(macroPromises).then((r) =>
      Object.assign(
        {},
        ...Array.from(
          aggregate(
            r.filter((x) => x.ok).map((x) => x.value),
            true,
          ).entries(),
        ).map(([k, v]) => ({[k]: v})),
      ),
    ),
  );
  ctx.addSqlPackages(
    Promise.all(sqlModulePromises).then((r) => [
      {
        // TODO(lalitm): DNS. This needs to be discussed before submitting.
        name: 'extension_servers',
        modules: Array.from(
          aggregate(
            r.filter((x) => x.ok).map((x) => x.value),
            false,
          ),
          ([name, sql]) => ({
            name,
            sql,
          }),
        ),
      },
    ]),
  );
  ctx.addProtoDescriptors(
    Promise.all(protoDescriptorPromises).then((r) =>
      r
        .filter((x) => x.ok)
        .map((x) => x.value)
        .flatMap((descs) => descs),
    ),
  );
}
