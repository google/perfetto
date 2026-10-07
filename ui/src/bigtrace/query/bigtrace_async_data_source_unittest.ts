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

import {afterEach, beforeEach, describe, expect, test, vi} from 'vitest';
import m from 'mithril';
import {BigtraceAsyncDataSource} from './bigtrace_async_data_source';
import {FETCH_DEBOUNCE_MS} from './fetch_scheduler';
import {
  type BigtraceQueryClient,
  BigtraceHttpError,
  QueryCancelledError,
} from './bigtrace_query_client';

// A client whose fetchResults always rejects with `err`.
function rejectingClient(err: Error): BigtraceQueryClient {
  return {
    fetchResults: vi.fn().mockRejectedValue(err),
  } as unknown as BigtraceQueryClient;
}

describe('BigtraceAsyncDataSource error handling', () => {
  test('an HTTP error surfaces the backend detail and status', async () => {
    const client = rejectingClient(
      new BigtraceHttpError(400, "unknown column 'foo'"),
    );
    const ds = new BigtraceAsyncDataSource('uid', client, () => 0);
    await ds.ensureResultsLoaded();
    // The view shows the detail, not the wrapped `HTTP 400: ...` message.
    expect(ds.getError()).toBe("unknown column 'foo'");
    expect(ds.getErrorStatus()).toBe(400);
  });

  test('a non-HTTP error surfaces its message with no status', async () => {
    const client = rejectingClient(new Error('network down'));
    const ds = new BigtraceAsyncDataSource('uid', client, () => 0);
    await ds.ensureResultsLoaded();
    expect(ds.getError()).toBe('network down');
    expect(ds.getErrorStatus()).toBeUndefined();
  });

  test('a cancelled fetch is swallowed (no error surfaced)', async () => {
    const client = rejectingClient(new QueryCancelledError());
    const ds = new BigtraceAsyncDataSource('uid', client, () => 0);
    await ds.ensureResultsLoaded();
    expect(ds.getError()).toBeNull();
    expect(ds.getErrorStatus()).toBeUndefined();
  });
});

// ---------------------------------------------------------------------------
// Fetch scheduling: debouncing a scroll, and superseding what it replaces.
// ---------------------------------------------------------------------------

interface Call {
  readonly limit: number;
  readonly offset: number;
  readonly signal?: AbortSignal;
  resolve: (page: unknown) => void;
}

// A client that hands back the calls it received, each resolvable by the test.
function manualClient(): {client: BigtraceQueryClient; calls: Call[]} {
  const calls: Call[] = [];
  const client = {
    fetchResults: vi.fn(
      (
        _uuid: string,
        limit: number,
        offset: number,
        signal?: AbortSignal,
      ): Promise<unknown> =>
        new Promise((resolve, reject) => {
          calls.push({limit, offset, signal, resolve});
          signal?.addEventListener('abort', () =>
            reject(new QueryCancelledError()),
          );
        }),
    ),
  } as unknown as BigtraceQueryClient;
  return {client, calls};
}

const PAGE = {
  rows: [{a: '1'}],
  columns: ['a'],
  totalFilteredRows: 5000,
  availableColumnNames: ['a'],
};

// The model the DataGrid hands a data source for one window.
function model(offset: number, limit: number) {
  return {
    mode: 'flat' as const,
    columns: [{field: 'a', alias: 'a'}],
    pagination: {offset, limit},
  };
}

describe('BigtraceAsyncDataSource fetch scheduling', () => {
  beforeEach(() => {
    vi.useFakeTimers();
    vi.spyOn(m, 'redraw').mockImplementation(() => {});
  });
  afterEach(() => {
    vi.useRealTimers();
    vi.restoreAllMocks();
  });

  // Settle the first window so later renders are range changes, not initial.
  async function loaded(ds: BigtraceAsyncDataSource, calls: Call[]) {
    ds.useRows(model(0, 100));
    calls[0].resolve(PAGE);
    await vi.advanceTimersByTimeAsync(0);
    calls.length = 0;
  }

  test('the first window is fetched at once, without waiting', () => {
    const {client, calls} = manualClient();
    const ds = new BigtraceAsyncDataSource('uid', client, () => 0);
    ds.useRows(model(0, 100));
    expect(calls).toHaveLength(1);
    expect(calls[0]).toMatchObject({offset: 0, limit: 100});
  });

  test('re-renders during the first fetch do not restart it', async () => {
    const {client, calls} = manualClient();
    const ds = new BigtraceAsyncDataSource('uid', client, () => 0);
    ds.useRows(model(0, 100));
    // Every fetch redraws, and needsInitial stays true until one succeeds —
    // so without request identity this loops.
    ds.useRows(model(0, 100));
    ds.useRows(model(0, 100));
    await vi.advanceTimersByTimeAsync(FETCH_DEBOUNCE_MS * 4);
    expect(calls).toHaveLength(1);
    expect(calls[0].signal?.aborted).toBe(false);
  });

  test('a burst of scroll ticks costs one fetch, for the last window', async () => {
    const {client, calls} = manualClient();
    const ds = new BigtraceAsyncDataSource('uid', client, () => 5000);
    await loaded(ds, calls);

    for (const offset of [200, 400, 600, 800]) {
      ds.useRows(model(offset, 100));
      await vi.advanceTimersByTimeAsync(FETCH_DEBOUNCE_MS / 3);
    }
    expect(calls).toHaveLength(0); // still settling

    await vi.advanceTimersByTimeAsync(FETCH_DEBOUNCE_MS);
    expect(calls).toHaveLength(1);
    expect(calls[0].offset).toBe(800);
  });

  test('a newer window aborts the request it replaces', async () => {
    const {client, calls} = manualClient();
    const ds = new BigtraceAsyncDataSource('uid', client, () => 5000);
    await loaded(ds, calls);

    ds.useRows(model(200, 100));
    await vi.advanceTimersByTimeAsync(FETCH_DEBOUNCE_MS);
    expect(calls).toHaveLength(1);
    const superseded = calls[0];

    // Scroll on while that one is still in flight.
    ds.useRows(model(900, 100));
    await vi.advanceTimersByTimeAsync(FETCH_DEBOUNCE_MS);
    expect(calls).toHaveLength(2);
    expect(superseded.signal?.aborted).toBe(true);
    expect(calls[1].signal?.aborted).toBe(false);
  });

  test('a superseded reply does not replace the newer window', async () => {
    const {client, calls} = manualClient();
    const ds = new BigtraceAsyncDataSource('uid', client, () => 5000);
    await loaded(ds, calls);

    ds.useRows(model(200, 100));
    await vi.advanceTimersByTimeAsync(FETCH_DEBOUNCE_MS);
    ds.useRows(model(900, 100));
    await vi.advanceTimersByTimeAsync(FETCH_DEBOUNCE_MS);

    // The abandoned request answers anyway, after the newer one landed.
    calls[1].resolve({...PAGE, rows: [{a: 'new'}]});
    await vi.advanceTimersByTimeAsync(0);
    calls[0].resolve({...PAGE, rows: [{a: 'stale'}]});
    await vi.advanceTimersByTimeAsync(0);

    expect(ds.useRows(model(900, 100)).rows).toEqual([{a: 'new'}]);
  });

  test('the grid reads as pending through the debounce window', async () => {
    const {client, calls} = manualClient();
    const ds = new BigtraceAsyncDataSource('uid', client, () => 5000);
    await loaded(ds, calls);

    ds.useRows(model(400, 100));
    // Nothing has been sent yet, but the grid must not read as settled.
    expect(calls).toHaveLength(0);
    expect(ds.useRows(model(400, 100)).isPending).toBe(true);
  });
});
