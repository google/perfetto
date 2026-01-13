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
  sqlModulesSchema,
} from './types';
import {resolveServerUrl} from './url_utils';
import {showModal} from '../../widgets/modal';
import {errResult, okResult, Result} from '../../base/result';
import {AppImpl} from '../../core/app_impl';

// =============================================================================
// Helpers
// =============================================================================

const FETCH_TIMEOUT_MS = 10000; // 10 seconds

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

// =============================================================================
// Loading
// =============================================================================

export async function loadManifest(
  serverUrl: string,
): Promise<Result<Manifest>> {
  return fetchJson(serverUrl + '/manifest', manifestSchema);
}

async function loadMacros(
  manifestResult: Result<Manifest>,
  canonicalUrl: string,
  module: string,
) {
  if (!manifestResult.ok) {
    return errResult(manifestResult.error);
  }
  const manifest = manifestResult.value;
  if (!manifest.modules.includes(module)) {
    return errResult(`Module '${module}' not found on server ${canonicalUrl}`);
  }
  // Check if macros are supported.
  if (!manifest.features.find((f) => f === 'macros')) {
    // Not supported, return empty list.
    return okResult([]);
  }
  const wrapper = await fetchJson(
    `${canonicalUrl}/modules/${module}/macros`,
    macrosSchema,
  );
  if (!wrapper.ok) {
    return errResult(wrapper.error);
  }
  // Validate that all macro IDs start with the namespace
  for (const macro of wrapper.value.macros) {
    if (!macro.id.startsWith(manifest.namespace + '.')) {
      return errResult(
        `Macro ID '${macro.id}' must start with namespace '${manifest.namespace}.'`,
      );
    }
  }
  return okResult(wrapper.value.macros);
}

async function loadSqlPackage(
  manifestResult: Result<Manifest>,
  canonicalUrl: string,
  module: string,
) {
  if (!manifestResult.ok) {
    return errResult(manifestResult.error);
  }
  const manifest = manifestResult.value;
  if (!manifest.modules.includes(module)) {
    return errResult(`Module '${module}' not found on server ${canonicalUrl}`);
  }
  // Check if sql_modules are supported.
  if (!manifest.features.find((f) => f === 'sql_modules')) {
    // Not supported, return empty list.
    return okResult([]);
  }
  const wrapper = await fetchJson(
    `${canonicalUrl}/modules/${module}/sql_modules`,
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
  canonicalUrl: string,
  module: string,
) {
  if (!manifestResult.ok) {
    return errResult(manifestResult.error);
  }
  const manifest = manifestResult.value;
  if (!manifest.modules.includes(module)) {
    return errResult(`Module '${module}' not found on server ${canonicalUrl}`);
  }
  // Check if proto_descriptors are supported.
  if (!manifest.features.find((f) => f === 'proto_descriptors')) {
    // Not supported, return empty list.
    return okResult([]);
  }
  const wrapper = await fetchJson(
    `${canonicalUrl}/modules/${module}/proto_descriptors`,
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
  for (const {url: rawUrl, enabledModules, enabled} of servers) {
    if (!enabled) {
      continue;
    }
    const url = resolveServerUrl(rawUrl);
    const manifest = loadManifest(url);
    for (const mod of enabledModules) {
      const macros = manifest.then((r) => loadMacros(r, url, mod));
      const sqlPackage = manifest.then((r) => loadSqlPackage(r, url, mod));
      const descs = manifest.then((r) => loadProtoDescriptors(r, url, mod));
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
