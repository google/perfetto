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

// Loads the distinct values of one trace-metadata column for the corpus
// filter pickers, with retry on transient backend failures and a readable
// account of the ones that stick.

import type {Filter} from '../../components/widgets/datagrid/model';
import {todayIso} from '../base/iso_date';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';
import type {SettingsBindings} from '../settings/tab_bound_setting';
import {
  BigtraceHttpError,
  BigtraceQueryClient,
  QueryCancelledError,
  toExperimentFilterSpec,
} from './bigtrace_query_client';
import {dateFilterToFilters} from './date_filter';

// How many values a picker asks for per request; past that it tells the user
// to type to narrow down rather than paging.
export const VALUE_LIST_LIMIT = 250;

export type ColumnValuesLoader = (
  column: string,
  traceFilters: ReadonlyArray<Filter>,
  search: string,
  signal?: AbortSignal,
) => Promise<ReadonlyArray<string>>;

// A value load that failed for a reason that may clear up on its own:
// backend load shedding (429/5xx) or a dropped connection (fetch rejects
// with a TypeError). Cancellations and 4xx answers are not; the latter won't
// change by asking again.
export function isTransientError(e: unknown): boolean {
  if (e instanceof QueryCancelledError) return false;
  if (e instanceof BigtraceHttpError) {
    return e.status === 429 || e.status >= 500;
  }
  return e instanceof TypeError;
}

// How many more times a transient failure is tried before it is shown, and
// the pause before the first of them (doubled each time, plus jitter so a
// burst of pickers doesn't knock on a shedding backend in lockstep).
export const RETRY_ATTEMPTS = 2;
export const RETRY_BASE_MS = 500;

export function retryDelayMs(attempt: number, random = Math.random): number {
  const base = RETRY_BASE_MS * 2 ** attempt;
  return base + random() * base * 0.5;
}

// Resolves after `ms`, or rejects with QueryCancelledError as soon as
// `signal` aborts, so a facet or search change doesn't wait out a backoff.
function sleep(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(new QueryCancelledError());
      return;
    }
    const onAbort = () => {
      clearTimeout(timer);
      reject(new QueryCancelledError());
    };
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, ms);
    signal?.addEventListener('abort', onAbort, {once: true});
  });
}

// Runs `attempt`, retrying transient failures up to RETRY_ATTEMPTS times
// with backoff. Anything else (or an abort) is thrown straight away. `wait`
// is injectable for tests.
export async function withRetry<T>(
  attempt: () => Promise<T>,
  signal?: AbortSignal,
  wait: (ms: number, signal?: AbortSignal) => Promise<void> = sleep,
): Promise<T> {
  for (let i = 0; ; i++) {
    try {
      return await attempt();
    } catch (e) {
      if (i >= RETRY_ATTEMPTS || !isTransientError(e)) throw e;
      await wait(retryDelayMs(i), signal);
    }
  }
}

// What to tell the user about a failed load: a one-line reason (the
// backend's own `error.message` when its body has the google.rpc shape, else
// the error's message) and the full text for a tooltip.
export function describeLoadError(e: unknown): {
  summary: string;
  detail: string;
} {
  const detail = e instanceof Error ? e.message : String(e);
  let summary = detail;
  if (e instanceof BigtraceHttpError) {
    summary = e.detail;
    try {
      const message = JSON.parse(e.detail)?.error?.message;
      if (typeof message === 'string' && message !== '') summary = message;
    } catch {
      // Not JSON; the raw detail is the best we have.
    }
    summary = `${summary} (HTTP ${e.status})`;
  } else if (e instanceof TypeError) {
    summary = 'Could not reach the backend.';
  }
  return {summary, detail};
}

// Values come from `/trace_metadata_column_values` over the tab's own trace
// source, experiment arm and upload-date range, so a picker lists what a run
// would see — and the date range keeps the backend's scan small. Transient
// backend failures are retried a couple of times before they surface.
export function columnValuesLoader(
  bindings: SettingsBindings,
): ColumnValuesLoader {
  return (column, traceFilters, search, signal) => {
    const endpoint = getBigtraceEndpoint();
    if (endpoint === '') return Promise.resolve([]);
    const client = new BigtraceQueryClient(endpoint);
    return withRetry(
      () =>
        client.listTraceMetadataColumnValues(
          bindings.getEffectiveSettings(),
          column,
          {
            search,
            traceFilters: [
              ...traceFilters,
              ...dateFilterToFilters(bindings.getDateFilter(), todayIso()),
            ],
            limit: VALUE_LIST_LIMIT,
            experimentFilter: toExperimentFilterSpec(
              bindings.getExperimentFilter(),
            ),
          },
          signal,
        ),
      signal,
    );
  };
}
