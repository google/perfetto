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
import {Macro} from '../../core/command_manager';
import {
  ExtensionServer,
  macrosSchema,
  Manifest,
  manifestSchema,
  ProtoDescriptor,
  protoDescriptorsSchema,
  SqlModule,
  sqlModulesSchema,
} from './types';
import {resolveServerUrl} from './url_utils';
import {showModal} from '../../widgets/modal';
import {errResult, okResult, Result} from '../../base/result';
import {AppImpl} from '../../core/app_impl';

// =============================================================================
// Helpers
// =============================================================================

const FETCH_TIMEOUT_MS = 5000; // 5 seconds

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
  canonicalUrl: string,
  module: string,
): Promise<Result<ReadonlyArray<Macro>>> {
  const wrapper = await fetchJson(
    `${canonicalUrl}/modules/${module}/macros`,
    macrosSchema,
  );
  if (!wrapper.ok) {
    return errResult(wrapper.error);
  }
  return okResult(wrapper.value.macros);
}

async function loadSqlModules(
  canonicalUrl: string,
  module: string,
): Promise<Result<ReadonlyArray<SqlModule>>> {
  const wrapper = await fetchJson(
    `${canonicalUrl}/modules/${module}/sql_modules`,
    sqlModulesSchema,
  );
  if (!wrapper.ok) {
    return errResult(wrapper.error);
  }
  return okResult(wrapper.value.sqlModules);
}

async function loadProtoDescriptors(
  canonicalUrl: string,
  module: string,
): Promise<Result<ReadonlyArray<ProtoDescriptor>>> {
  const wrapper = await fetchJson(
    `${canonicalUrl}/modules/${module}/proto_descriptors`,
    protoDescriptorsSchema,
  );
  if (!wrapper.ok) {
    return errResult(wrapper.error);
  }
  return okResult(wrapper.value.descriptors);
}

// =============================================================================
// Initialization
// =============================================================================

function maybeLoadMacros(
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
  return loadMacros(canonicalUrl, module);
}

function maybeLoadSqlPackage(
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
  return loadSqlModules(canonicalUrl, module).then((modules) => {
    if (!modules.ok) {
      return errResult(modules.error);
    }
    return okResult([
      {
        name: manifest.namespace,
        modules: modules.value,
      },
    ]);
  });
}

function maybeLoadProtoDescriptors(
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
  return loadProtoDescriptors(canonicalUrl, module);
}

// Initializes extension servers by fetching manifests (synchronously) and
// loading extensions (asynchronously). This function should be called early
// in app initialization.
export function initializeExtensions(
  ctx: AppImpl,
  servers: ReadonlyArray<ExtensionServer>,
) {
  const results = [];
  for (const {url, enabledModules} of servers) {
    const canonicalUrl = resolveServerUrl(url);
    const manifest = loadManifest(canonicalUrl);
    for (const module of enabledModules) {
      const macros = manifest.then((r) =>
        maybeLoadMacros(r, canonicalUrl, module),
      );
      const sqlPackage = manifest.then((r) =>
        maybeLoadSqlPackage(r, canonicalUrl, module),
      );
      const protoDescriptors = manifest.then((r) =>
        maybeLoadProtoDescriptors(r, canonicalUrl, module),
      );
      ctx.addMacros(macros.then((r) => (r.ok ? r.value : [])));
      ctx.addSqlPackages(sqlPackage.then((r) => (r.ok ? r.value : [])));
      ctx.addProtoDescriptors(
        protoDescriptors.then((r) => (r.ok ? r.value : [])),
      );
      results.push(macros, sqlPackage, protoDescriptors);
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
