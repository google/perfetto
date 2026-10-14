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

// Features of the configured backend, from `GET /`. 'duckdb' is offered only
// by the local server (tools/bigtrace_server.py).
let known:
  | {endpoint: string; features: ReadonlySet<string>; retryAt?: number}
  | undefined;
let fetching: string | undefined;

// Whether the configured backend advertises `feature`. Safe to call from
// view(): it reports false until the answer lands, then redraws. An
// unreachable backend (e.g. a local server started after the UI) is asked
// again after a while; an HTTP error means the backend has no `GET /`.
export function serverHasFeature(feature: 'duckdb'): boolean {
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
  return known?.endpoint === endpoint && known.features.has(feature);
}
