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
  MacrosSchema,
  Manifest,
  ManifestSchema,
  ProtoDescriptorsSchema,
  SqlModulesSchema,
} from './types';
import {normalizeServerKey, resolveServerUrl} from './url_utils';
import {showModal} from '../../widgets/modal';

const FETCH_TIMEOUT_MS = 5000; // 5 seconds

// =============================================================================
// Runtime State Management
// =============================================================================

async function loadServerState(
  server: ExtensionServer,
): Promise<ExtensionServerState> {
  const canonicalUrl = resolveServerUrl(server.url);
  const serverKey = normalizeServerKey(canonicalUrl);

  const fetchManifestOrUndefined = async (canonicalUrl: string) => {
    try {
      return await fetchManifest(canonicalUrl);
    } catch (e) {
      return undefined;
    }
  };
  const manifest = await fetchManifestOrUndefined(canonicalUrl);
  return {
    url: server.url,
    enabledModules: server.enabledModules,
    enabled: server.enabled,
    canonicalUrl,
    serverKey,
    displayName: manifest?.name ?? canonicalUrl,
    availableModules: manifest?.modules ?? [],
    lastFetchError: manifest ? undefined : 'Failed to fetch manifest',
  };
}

// Loads runtime state from extension server configs and fetched manifests.
export async function loadServerStates(
  servers: ExtensionServer[],
): Promise<ExtensionServerState[]> {
  return Promise.all(servers.map(loadServerState));
}

// =============================================================================
// Fetching
// =============================================================================

async function fetchJson<T extends z.ZodTypeAny>(
  url: string,
  schema: T,
): Promise<z.infer<T>> {
  const response = await fetchWithTimeout(url, {}, FETCH_TIMEOUT_MS);
  if (!response.ok) {
    throw new Error(`Fetch failed: ${url} returned ${response.status}`);
  }
  const json = await response.json();
  const result = schema.safeParse(json);
  if (!result.success) {
    throw new Error(`Invalid response from ${url}: ${result.error.message}`);
  }
  return result.data;
}

// Fetches manifest, returning undefined on error (for graceful degradation
// when configuring servers).
export async function fetchManifest(serverUrl: string): Promise<Manifest> {
  return await fetchJson(serverUrl + '/manifest.json', ManifestSchema);
}

// =============================================================================
// Loading
// =============================================================================

async function loadModuleMacros(
  state: ExtensionServerState,
  module: string,
): Promise<Array<[string, CommandInvocation[]]>> {
  try {
    const wrapper = await fetchJson(
      `${state.canonicalUrl}/modules/${module}/macros`,
      MacrosSchema,
    );
    return Object.entries(wrapper.macros)
      .sort()
      .map(([name, commands]) => [
        `[${state.serverKey} ${module}] ${name}`,
        commands,
      ]);
  } catch (e) {
    await showModal({
      title: 'Fetching server extensions failed',
      content: `Failed to load macros from ${state.canonicalUrl}/${module}: ${e}`,
    });
    return [];
  }
}

async function loadModuleSqlModules(
  state: ExtensionServerState,
  module: string,
): Promise<Array<[string, string]>> {
  try {
    const wrapper = await fetchJson(
      `${state.canonicalUrl}/modules/${module}/sql_modules`,
      SqlModulesSchema,
    );
    return Object.entries(wrapper.modules).sort();
  } catch (e) {
    await showModal({
      title: 'Fetching server extensions failed',
      content: `Failed to load sql from ${state.canonicalUrl}/${module}: ${e}`,
    });
    return [];
  }
}

async function loadModuleProtoDescriptors(
  state: ExtensionServerState,
  module: string,
): Promise<ReadonlyArray<string>> {
  try {
    const wrapper = await fetchJson(
      `${state.canonicalUrl}/modules/${module}/proto_descriptors`,
      ProtoDescriptorsSchema,
    );
    return wrapper.descriptors.sort();
  } catch (e) {
    await showModal({
      title: 'Fetching server extensions failed',
      content: `Failed to load protos from ${state.canonicalUrl}/${module}: ${e}`,
    });
    return [];
  }
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
//
// Returns:
// - states: Extension server states with manifests (needed for CSP)
// - macrosPromise: Resolves when macros are loaded
// - sqlModulesPromise: Resolves when SQL modules are loaded
// - protoDescriptorsPromise: Resolves when proto descriptors are loaded
export async function initializeExtensions(
  servers: ExtensionServer[],
): Promise<ExtensionInitializationResult> {
  // Load server states (fetches manifests) - this is blocking
  const states = await loadServerStates(servers);

  // Sort alphabetically by serverKey for deterministic ordering
  const sorted = states
    .filter((s) => s.enabled)
    .sort((a, b) => a.serverKey.localeCompare(b.serverKey));

  // Fire off all loads in parallel
  const macroPromises: Promise<ReadonlyArray<[string, CommandInvocation[]]>>[] =
    [];
  const sqlModulePromises: Promise<ReadonlyArray<[string, string]>>[] = [];
  const protoDescriptorPromises: Promise<ReadonlyArray<string>>[] = [];

  for (const state of sorted) {
    for (const module of [...state.enabledModules].sort()) {
      macroPromises.push(loadModuleMacros(state, module));
      sqlModulePromises.push(loadModuleSqlModules(state, module));
      protoDescriptorPromises.push(loadModuleProtoDescriptors(state, module));
    }
  }

  // Aggregate results into maps (macros overwrite, others first-wins)
  const macrosPromise = Promise.all(macroPromises).then((r) =>
    aggregate(r, true),
  );
  const sqlModulesPromise = Promise.all(sqlModulePromises).then((r) =>
    aggregate(r, false),
  );
  const protoDescriptorsPromise = Promise.all(protoDescriptorPromises).then(
    (r) => r.flatMap((descs) => descs),
  );
  return {states, macrosPromise, sqlModulesPromise, protoDescriptorsPromise};
}
