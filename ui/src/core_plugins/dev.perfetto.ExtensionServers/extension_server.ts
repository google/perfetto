// Copyright (C) 2024 The Android Open Source Project
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
import {CommandInvocation} from '../../core/command_manager';
import {
  ExtensionServer,
  ExtensionServerState,
  Macros,
  MacrosSchema,
  Manifest,
  ManifestSchema,
  ProtoDescriptor,
  ProtoDescriptors,
  ProtoDescriptorsSchema,
  SqlModulesSchema,
} from './types';
import {normalizeServerKey, resolveServerUrl} from './url_utils';

// =============================================================================
// Runtime State Management
// =============================================================================

async function buildServerState(
  server: ExtensionServer,
): Promise<ExtensionServerState> {
  const resolvedUrl = resolveServerUrl(server.url);
  const serverKey = normalizeServerKey(resolvedUrl);
  const manifest = await fetchManifest(resolvedUrl);

  return {
    url: server.url,
    selectedModules: server.selectedModules,
    enabled: server.enabled,
    resolvedUrl,
    serverKey,
    displayName: manifest?.name ?? server.url,
    availableModules: manifest?.modules ?? [],
    cspAllowUrls: manifest?.csp_allow ?? [],
    lastFetchStatus: manifest ? 'success' : 'error',
  };
}

/**
 * Builds runtime state from extension server configs and fetched manifests.
 */
export async function buildServerStates(
  servers: ExtensionServer[],
): Promise<ExtensionServerState[]> {
  return Promise.all(servers.map(buildServerState));
}

// =============================================================================
// Fetching
// =============================================================================

async function fetchJson<T>(
  url: string,
  schema: z.ZodSchema,
): Promise<T | undefined> {
  try {
    const response = await fetch(url);
    if (!response.ok) return undefined;

    const json = await response.json();
    const result = schema.safeParse(json);
    return result.success ? (result.data as T) : undefined;
  } catch {
    return undefined;
  }
}

export async function fetchManifest(
  serverUrl: string,
): Promise<Manifest | undefined> {
  return fetchJson(`${serverUrl}/manifest.json`, ManifestSchema);
}

// =============================================================================
// Loading
// =============================================================================

async function loadExtension<T>(
  state: ExtensionServerState,
  module: string,
  resourceType: 'macros' | 'sql_modules' | 'proto_descriptors',
  schema: z.ZodSchema,
  transformKey?: (
    state: ExtensionServerState,
    module: string,
    key: string,
  ) => string,
): Promise<Array<[string, T]>> {
  const data = await fetchJson<Record<string, T>>(
    `${state.resolvedUrl}/modules/${module}/${resourceType}`,
    schema,
  );
  if (!data) return [];

  return Object.entries(data)
    .sort()
    .map(([key, value]) => [
      transformKey ? transformKey(state, module, key) : key,
      value,
    ]);
}

function aggregate<T>(
  results: Array<Array<[string, T]>>,
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
  macrosPromise: Promise<Map<string, Macros[string]>>;
  sqlModulesPromise: Promise<Map<string, string>>;
  protoDescriptorsPromise: Promise<Map<string, ProtoDescriptors[string]>>;
}

/**
 * Initializes extension servers by fetching manifests (synchronously) and
 * loading extensions (asynchronously). This function should be called early
 * in app initialization.
 *
 * Returns:
 * - states: Extension server states with manifests (needed for CSP)
 * - macrosPromise: Resolves when macros are loaded
 * - sqlModulesPromise: Resolves when SQL modules are loaded
 * - protoDescriptorsPromise: Resolves when proto descriptors are loaded
 */
export async function initializeExtensions(
  servers: ExtensionServer[],
): Promise<ExtensionInitializationResult> {
  // Build server states (fetches manifests) - this is blocking
  const states = await buildServerStates(servers);

  // Sort alphabetically by serverKey for deterministic ordering
  const sorted = states
    .filter((s) => s.enabled)
    .sort((a, b) => a.serverKey.localeCompare(b.serverKey));

  // Fire off all loads in parallel
  const macroPromises: Promise<Array<[string, CommandInvocation[]]>>[] = [];
  const sqlModulePromises: Promise<Array<[string, string]>>[] = [];
  const protoDescriptorPromises: Promise<Array<[string, ProtoDescriptor]>>[] =
    [];

  for (const state of sorted) {
    for (const module of [...state.selectedModules].sort()) {
      macroPromises.push(
        loadExtension<CommandInvocation[]>(
          state,
          module,
          'macros',
          MacrosSchema,
          (s, m, name) => `[${s.serverKey} ${m}] ${name}`,
        ),
      );
      sqlModulePromises.push(
        loadExtension<string>(state, module, 'sql_modules', SqlModulesSchema),
      );
      protoDescriptorPromises.push(
        loadExtension<ProtoDescriptor>(
          state,
          module,
          'proto_descriptors',
          ProtoDescriptorsSchema,
        ),
      );
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
    (r) => aggregate(r, false),
  );

  return {states, macrosPromise, sqlModulesPromise, protoDescriptorsPromise};
}
