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

import {BigtraceAsyncDataSource} from './bigtrace_async_data_source';
import {BigtraceQueryClient} from './bigtrace_query_client';
import {DataSourceModel} from '../../components/widgets/datagrid/data_source';
import {Filter} from '../../components/widgets/datagrid/model';

// `useRows` triggers an async fetch via `fetchMoreRows` which awaits
// `queryClient.fetchResults()`. To observe the result we have to wait
// past at least one microtask. The two-tick wait is enough for the
// promise to settle and the data-source's bookkeeping (`isFetching`,
// `loadedRows`, `filteredTotalRows`) to update.
function flushAsync() {
  return new Promise<void>((r) => setTimeout(r, 0));
}

// The data source receives a `DataSourceModel` from the DataGrid
// widget. Real models are deeply nested; we hand-roll a minimal
// fake that exposes only the fields the data source reads
// (`pagination`, `sort`, `filters`, `columns`). `as never` shields
// the test from upstream model shape changes that don't affect the
// data source's contract.
function fakeModel(opts: {
  filters?: ReadonlyArray<Filter>;
  columns?: ReadonlyArray<{field: string; alias?: string}>;
  offset?: number;
  limit?: number;
}): DataSourceModel {
  return {
    pagination: {offset: opts.offset ?? 0, limit: opts.limit ?? 10},
    filters: opts.filters,
    columns: opts.columns,
  } as never;
}

// Build a stub `BigtraceQueryClient` that records every
// `fetchResults` call and returns a configurable response. We don't
// stub other methods because the data source only calls
// `fetchResults` from this code path.
interface MockClient {
  client: BigtraceQueryClient;
  setNextResponse: (response: {
    rows?: ReadonlyArray<Record<string, unknown>>;
    columns?: ReadonlyArray<string>;
    totalFilteredRows?: number;
  }) => void;
  calls: () => Array<{
    uuid: string;
    limit: number;
    offset: number;
    orderBy: string | undefined;
    filter: ReadonlyArray<Filter> | undefined;
  }>;
}
function makeMockClient(): MockClient {
  let next: {
    rows: ReadonlyArray<Record<string, unknown>>;
    columns: ReadonlyArray<string>;
    totalFilteredRows: number;
  } = {rows: [], columns: [], totalFilteredRows: 0};
  const calls: Array<{
    uuid: string;
    limit: number;
    offset: number;
    orderBy: string | undefined;
    filter: ReadonlyArray<Filter> | undefined;
  }> = [];
  const fetchResults = jest.fn(
    async (
      uuid: string,
      limit: number,
      offset: number,
      _signal?: AbortSignal,
      orderBy?: string,
      filter?: ReadonlyArray<Filter>,
    ) => {
      calls.push({uuid, limit, offset, orderBy, filter});
      return {
        rows: next.rows,
        columns: next.columns,
        totalFilteredRows: next.totalFilteredRows,
      };
    },
  );
  return {
    client: {fetchResults} as unknown as BigtraceQueryClient,
    setNextResponse: (r) =>
      (next = {
        rows: r.rows ?? [],
        columns: r.columns ?? [],
        totalFilteredRows: r.totalFilteredRows ?? 0,
      }),
    calls: () => calls,
  };
}

describe('BigtraceAsyncDataSource — alias→field remap', () => {
  test('formatFilter rewrites widget alias to backend field', async () => {
    const mock = makeMockClient();
    mock.setNextResponse({totalFilteredRows: 7});
    const ds = new BigtraceAsyncDataSource('test-uuid', mock.client, () => 100);
    ds.useRows(
      fakeModel({
        filters: [{field: 'displayName', op: '=', value: 'kernel'}],
        columns: [{alias: 'displayName', field: 'real_name'}],
      }),
    );
    await flushAsync();
    expect(mock.calls()).toHaveLength(1);
    expect(mock.calls()[0].filter).toEqual([
      {field: 'real_name', op: '=', value: 'kernel'},
    ]);
  });

  test('formatFilter falls back to alias when no column mapping is provided', async () => {
    // Defensive path: if `model.columns` is missing the entry, the
    // data source should pass through the alias as the field rather
    // than dropping the filter.
    const mock = makeMockClient();
    const ds = new BigtraceAsyncDataSource('uid', mock.client, () => 0);
    ds.useRows(
      fakeModel({
        filters: [{field: 'orphan_field', op: '=', value: 1}],
        columns: [],
      }),
    );
    await flushAsync();
    expect(mock.calls()[0].filter).toEqual([
      {field: 'orphan_field', op: '=', value: 1},
    ]);
  });
});

describe('BigtraceAsyncDataSource — useRows trigger logic', () => {
  test('does not refetch when called twice with the same filter', async () => {
    const mock = makeMockClient();
    const ds = new BigtraceAsyncDataSource('uid', mock.client, () => 0);
    const model = fakeModel({
      filters: [{field: 'a', op: '=', value: 1}],
      columns: [{alias: 'a', field: 'a'}],
    });
    ds.useRows(model);
    await flushAsync();
    ds.useRows(model);
    await flushAsync();
    expect(mock.calls()).toHaveLength(1);
  });

  test('refetches with the new filter when filter changes', async () => {
    const mock = makeMockClient();
    const ds = new BigtraceAsyncDataSource('uid', mock.client, () => 0);
    ds.useRows(
      fakeModel({
        filters: [{field: 'a', op: '=', value: 1}],
        columns: [{alias: 'a', field: 'a'}],
      }),
    );
    await flushAsync();
    ds.useRows(
      fakeModel({
        filters: [{field: 'a', op: '=', value: 2}],
        columns: [{alias: 'a', field: 'a'}],
      }),
    );
    await flushAsync();
    expect(mock.calls()).toHaveLength(2);
    expect(mock.calls()[1].filter).toEqual([{field: 'a', op: '=', value: 2}]);
  });

  test('semantically-equal filters built in different key orders do not refetch', async () => {
    // `currentFilterKey` is canonical (key-sorted) — this test pins
    // that property at the data-source level so a future change
    // that breaks canonicalization shows up as a noisy refetch
    // instead of a silent perf regression.
    const mock = makeMockClient();
    const ds = new BigtraceAsyncDataSource('uid', mock.client, () => 0);
    ds.useRows(
      fakeModel({
        filters: [{field: 'a', op: '=', value: 1}],
        columns: [{alias: 'a', field: 'a'}],
      }),
    );
    await flushAsync();
    // Construct a filter object via JSON.parse so the engine
    // preserves a non-natural insertion order.
    const reordered = JSON.parse('{"value":1,"op":"=","field":"a"}');
    ds.useRows(
      fakeModel({
        filters: [reordered],
        columns: [{alias: 'a', field: 'a'}],
      }),
    );
    await flushAsync();
    expect(mock.calls()).toHaveLength(1);
  });
});

describe('BigtraceAsyncDataSource — filteredTotalRows flow', () => {
  test('reports backend-supplied count after first fetch', async () => {
    const mock = makeMockClient();
    mock.setNextResponse({totalFilteredRows: 42});
    const ds = new BigtraceAsyncDataSource(
      'uid',
      mock.client,
      () => 1000, // unfiltered fallback
    );
    const model = fakeModel({
      filters: [{field: 'a', op: '=', value: 1}],
      columns: [{alias: 'a', field: 'a'}],
    });
    ds.useRows(model);
    await flushAsync();
    const result = ds.useRows(model);
    expect(result.totalRows).toBe(42);
  });

  test('falls back to getTotalRows() before any fetch completes', () => {
    const mock = makeMockClient();
    const ds = new BigtraceAsyncDataSource('uid', mock.client, () => 1000);
    // First useRows call: `filteredTotalRows` is still undefined,
    // so totalRows must come from the fallback.
    const result = ds.useRows(fakeModel({}));
    expect(result.totalRows).toBe(1000);
  });

  test('clears filteredTotalRows on filter change so scrollbar reverts to fallback briefly', async () => {
    const mock = makeMockClient();
    mock.setNextResponse({totalFilteredRows: 50});
    const ds = new BigtraceAsyncDataSource('uid', mock.client, () => 9999);
    ds.useRows(
      fakeModel({
        filters: [{field: 'a', op: '=', value: 1}],
        columns: [{alias: 'a', field: 'a'}],
      }),
    );
    await flushAsync();
    expect(
      ds.useRows(
        fakeModel({
          filters: [{field: 'a', op: '=', value: 1}],
          columns: [{alias: 'a', field: 'a'}],
        }),
      ).totalRows,
    ).toBe(50);
    // Stage a new response but DON'T flush — the change-detection
    // path should clear filteredTotalRows synchronously, so the
    // very next useRows() (still no response back) reports the
    // fallback value rather than the stale 50.
    mock.setNextResponse({totalFilteredRows: 7});
    const next = ds.useRows(
      fakeModel({
        filters: [{field: 'a', op: '=', value: 2}], // different value → filter changed
        columns: [{alias: 'a', field: 'a'}],
      }),
    );
    expect(next.totalRows).toBe(9999);
    await flushAsync();
    // After the new response lands, totalRows reflects the new count.
    expect(
      ds.useRows(
        fakeModel({
          filters: [{field: 'a', op: '=', value: 2}],
          columns: [{alias: 'a', field: 'a'}],
        }),
      ).totalRows,
    ).toBe(7);
  });
});
