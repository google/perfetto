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

import {beforeEach, describe, expect, test, vi} from 'vitest';
import type {BigtraceQueryClient} from './bigtrace_query_client';
import {PollingController} from './polling_controller';
import {queryStore} from './query_store';
import {addQuery, QueryTabsState} from '../pages/query_tabs_state';

describe('PollingController', () => {
  beforeEach(() => localStorage.clear());

  test('polls every query of a multi-query run until all are done', async () => {
    vi.useFakeTimers();
    let status = 'IN_PROGRESS';
    const client = {
      getStatus: vi.fn(async () => ({status, processedRows: 7})),
      getQueryExecution: vi.fn(async () => ({})),
    } as unknown as BigtraceQueryClient;
    const tab = new QueryTabsState().tabs[0];
    addQuery(tab);
    tab.queries.forEach((q, i) => {
      q.queryUuid = `u${i}`;
      q.execution = queryStore.getOrCreate(`u${i}`);
    });
    tab.queryClient = client;
    tab.isLoading = true;
    const cb = {redraw: vi.fn(), onHistoryChanged: vi.fn()};

    new PollingController(cb).start(tab, ['u0', 'u1']);
    await vi.advanceTimersByTimeAsync(0);
    expect(client.getStatus).toHaveBeenCalledTimes(2);
    expect(tab.isLoading).toBe(true);

    status = 'SUCCESS';
    await vi.advanceTimersByTimeAsync(3000);
    expect(client.getStatus).toHaveBeenCalledTimes(4);
    expect(tab.isLoading).toBe(false);
    expect(tab.queries.map((q) => q.execution?.processedRows)).toEqual([7, 7]);
    expect(client.getQueryExecution).toHaveBeenCalledTimes(2);
    expect(cb.onHistoryChanged).toHaveBeenCalledTimes(1);
    vi.useRealTimers();
  });
});
