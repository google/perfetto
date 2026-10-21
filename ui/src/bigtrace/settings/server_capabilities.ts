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

import m from 'mithril';
import {
  BigtraceHttpError,
  BigtraceQueryClient,
  QueryNotFoundError,
} from '../query/bigtrace_query_client';
import {getBigtraceEndpoint} from './endpoint_storage';

// Features of the configured backend, from `GET /`. Only the local server
// (tools/bigtrace_server.py) offers any:
// - 'duckdb': DuckDB SQL over result tables (the PerfettoSQL / DuckDB SQL
//   toggle);
// - 'multi_query': several queries in one pass over the traces (Add Query).
export type ServerFeature = 'duckdb' | 'multi_query';

let known:
  | {endpoint: string; features: ReadonlySet<string>; retryAt?: number}
  | undefined;
let fetching: string | undefined;

// The features of the configured backend, or undefined until it has been
// asked. Safe to call from view(): it redraws when the answer lands. An
// unreachable backend (e.g. a local server started after the UI) is asked
// again after a while; an HTTP error means the backend has no `GET /`, so
// none.
function serverFeatures(): ReadonlySet<string> | undefined {
  const endpoint = getBigtraceEndpoint().trim();
  const stale =
    known?.endpoint !== endpoint ||
    (known.retryAt !== undefined && Date.now() >= known.retryAt);
  if (endpoint !== '' && stale && fetching !== endpoint) {
    fetching = endpoint;
    new BigtraceQueryClient(endpoint)
      .getServerInfo()
      .then(
        (info) => ({features: new Set(info.features ?? [])}),
        (e) => {
          const answered =
            e instanceof BigtraceHttpError || e instanceof QueryNotFoundError;
          return {
            features: new Set<string>(),
            retryAt: answered ? undefined : Date.now() + 10_000,
          };
        },
      )
      .then((result) => {
        fetching = undefined;
        known = {endpoint, ...result};
        m.redraw();
      });
  }
  return known?.endpoint === endpoint ? known.features : undefined;
}

// Whether the configured backend advertises `feature`; false until known.
export function serverHasFeature(feature: ServerFeature): boolean {
  return serverFeatures()?.has(feature) ?? false;
}

// Whether the configured backend is known not to offer `feature`; false
// until known.
export function serverLacksFeature(feature: ServerFeature): boolean {
  const features = serverFeatures();
  return features !== undefined && !features.has(feature);
}
