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

// One microtask is enough for fetchResults to settle and bookkeeping to update.
function flushAsync() {
  return new Promise<void>((r) => setTimeout(r, 0));
}

// Minimal model exposing only the fields the data source reads.
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

// Stub client recording every fetchResults call with a configurable response.
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
    // Missing column entry → pass alias through (don't drop the filter).
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
    // Pins canonical key-sort at the data-source level.
    const mock = makeMockClient();
    const ds = new BigtraceAsyncDataSource('uid', mock.client, () => 0);
    ds.useRows(
      fakeModel({
        filters: [{field: 'a', op: '=', value: 1}],
        columns: [{alias: 'a', field: 'a'}],
      }),
    );
    await flushAsync();
    // JSON.parse preserves a non-natural insertion order.
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
    // Pre-first-fetch: filteredTotalRows undefined → use fallback.
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
    // Don't flush — filter-change clears filteredTotalRows synchronously,
    // so the next useRows() reports the fallback, not the stale 50.
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
