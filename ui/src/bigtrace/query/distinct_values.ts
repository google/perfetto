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

import {
  AsyncMemo,
  type AsyncMemoResult,
  TASK_CANCELLED,
} from '../../base/async_memo';
import type {SqlValue} from '../../trace_processor/query_result';
import {SEARCH_DEBOUNCE_MS} from './fetch_scheduler';
import {sleep, withRetry} from './retry';

// What a grid's Equals / Not equals picker asks for. `scope` is everything
// else that decides which values exist (the grid's filters, the trace source,
// ...) as one string.
export interface DistinctValuesRequest {
  readonly column: string;
  readonly search: string;
  readonly scope: string;
}

// The values found for one search.
interface Reply {
  readonly search: string;
  readonly values: ReadonlyArray<string>;
}

// The values behind a grid's Equals / Not equals picker
// (DataSource.useDistinctValues). Both grids use it, so their pickers behave
// the same. The picker asks on every render and needs an answer right away,
// so the latest reply is kept until a newer one replaces it. Typing waits for
// a pause before a search is sent, a request a newer one replaces is
// cancelled, and transient backend failures are retried.
export class DistinctValues {
  private readonly memo = new AsyncMemo<Reply>();
  // The request on its way, so a newer one can cancel it.
  private inFlight?: {
    readonly key: string;
    readonly controller: AbortController;
  };

  // A new `version` (e.g. the growing row count of a run in progress)
  // refetches without cancelling the reply on its way.
  use(
    request: DistinctValuesRequest,
    fetch: (signal: AbortSignal) => Promise<ReadonlyArray<string>>,
    version = 0,
  ): AsyncMemoResult<readonly SqlValue[]> {
    const {column, search, scope} = request;
    const key = JSON.stringify([column, search, scope]);
    // Requests run one at a time, so a superseded one would hold up the next.
    if (this.inFlight !== undefined && this.inFlight.key !== key) {
      this.inFlight.controller.abort();
    }
    const {data: reply, isPending} = this.memo.use({
      key: {column, search, scope, version},
      retainOn: ['search', 'version'],
      compute: async () => {
        const controller = new AbortController();
        this.inFlight = {key, controller};
        const {signal} = controller;
        try {
          if (search !== '') await sleep(SEARCH_DEBOUNCE_MS, signal);
          return {search, values: await withRetry(() => fetch(signal), signal)};
        } catch {
          // Cancelled: keep nothing. Failed: the picker has no error state,
          // and an empty list beats an exception out of the grid's render.
          return signal.aborted ? TASK_CANCELLED : {search, values: []};
        } finally {
          this.inFlight = undefined;
        }
      },
    });
    // While a newer search loads, the picker narrows the held reply. That only
    // fits a longer search; for a shorter one (a deleted letter, a reopened
    // picker) it would show part of the list as if it were all of it.
    if (
      reply === undefined ||
      !search.toLowerCase().includes(reply.search.toLowerCase())
    ) {
      return {data: undefined, isPending: true};
    }
    return {data: reply.values, isPending};
  }

  // Forget the reply and cancel the request on its way.
  invalidate(): void {
    this.inFlight?.controller.abort();
    this.memo.invalidate();
  }
}
