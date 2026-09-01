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
import m from 'mithril';
import {BigtraceTraceListDataSource} from './bigtrace_trace_list_data_source';
import {
  type BigtraceQueryClient,
  QueryCancelledError,
  type QueryResultPage,
} from './bigtrace_query_client';
import type {DataSourceModel} from '../../components/widgets/datagrid/data_source';
import type {Filter} from '../../components/widgets/datagrid/model';
import type {SettingFilter} from '../settings/settings_types';

const SETTINGS: SettingFilter[] = [
  {settingId: 'trace_directory', values: ['/d'], category: 'TRACE_ADDRESS'},
];

const PAGE: QueryResultPage = {
  rows: [{file_name: 'a.pftrace', size_bytes: '10'}],
  columns: ['file_name', 'size_bytes'],
  totalFilteredRows: 1,
};

// Records the args of each listTraceMetadata call; resolves with `page`
// (or rejects with `reject` when set).
interface Recorded {
  settings: ReadonlyArray<SettingFilter>;
  limit: number;
  offset: number;
  orderBy?: string;
  filter?: ReadonlyArray<Filter>;
  columns?: ReadonlyArray<string>;
}

function fakeClient(opts?: {page?: QueryResultPage; reject?: Error}) {
  const calls: Recorded[] = [];
  const listTraceMetadata = vi.fn(
    async (
      settings: ReadonlyArray<SettingFilter>,
      limit: number,
      offset: number,
      _signal?: AbortSignal,
      orderBy?: string,
      filter?: ReadonlyArray<Filter>,
      columns?: ReadonlyArray<string>,
    ) => {
      calls.push({settings, limit, offset, orderBy, filter, columns});
      if (opts?.reject) throw opts.reject;
      return opts?.page ?? PAGE;
    },
  );
  const client = {listTraceMetadata} as unknown as BigtraceQueryClient;
  return {client, calls};
}

function flatModel(opts: {
  offset?: number;
  limit?: number;
  filters?: readonly Filter[];
  sort?: {alias: string; direction: 'ASC' | 'DESC'};
  columns?: readonly string[];
}): DataSourceModel {
  return {
    mode: 'flat',
    columns: (opts.columns ?? []).map((f) => ({field: f, alias: f})),
    filters: opts.filters,
    sort: opts.sort,
    pagination:
      opts.limit !== undefined
        ? {offset: opts.offset ?? 0, limit: opts.limit}
        : undefined,
  } as DataSourceModel;
}

// Let the fire-and-forget fetch kicked off by useRows settle.
function flush(): Promise<void> {
  return new Promise((r) => setTimeout(r, 0));
}

describe('BigtraceTraceListDataSource', () => {
  beforeEach(() => {
    // The data source calls m.redraw() around each fetch; no component is
    // mounted in the test, so stub it out.
    vi.spyOn(m, 'redraw').mockImplementation(() => {});
  });

  test('first render fetches with the fallback page size when limit is 0', async () => {
    // The DataGrid's first render can arrive with limit=0; we still fetch (to
    // pull back the columns) using a fallback page size, matching the sibling
    // BigtraceAsyncDataSource.
    const {client, calls} = fakeClient();
    const ds = new BigtraceTraceListDataSource(client, () => SETTINGS);
    ds.useRows(flatModel({limit: 0}));
    await flush();
    expect(calls).toHaveLength(1);
    expect(calls[0].limit).toBe(100);
  });

  test('initial fetch ships settings + paging and exposes the rows', async () => {
    const {client, calls} = fakeClient();
    const ds = new BigtraceTraceListDataSource(client, () => SETTINGS);
    ds.useRows(flatModel({limit: 100}));
    await flush();

    expect(calls).toHaveLength(1);
    expect(calls[0].settings).toBe(SETTINGS);
    expect(calls[0].limit).toBe(100);
    expect(calls[0].offset).toBe(0);
    expect(calls[0].orderBy).toBe('');

    const rows = ds.useRows(flatModel({limit: 100}));
    expect(rows.rows).toEqual(PAGE.rows);
    expect(rows.totalRows).toBe(1);
    expect(ds.getColumns()).toEqual(['file_name', 'size_bytes']);
  });

  test('formats order_by from the grid sort (no alias remap)', async () => {
    const {client, calls} = fakeClient();
    const ds = new BigtraceTraceListDataSource(client, () => SETTINGS);
    ds.useRows(flatModel({limit: 100}));
    await flush();
    ds.useRows(
      flatModel({limit: 100, sort: {alias: 'size_bytes', direction: 'DESC'}}),
    );
    await flush();

    expect(calls).toHaveLength(2);
    expect(calls[1].orderBy).toBe('size_bytes desc');
  });

  test('ships the visible-column projection and the filter array', async () => {
    const {client, calls} = fakeClient();
    const ds = new BigtraceTraceListDataSource(client, () => SETTINGS);
    const filters: Filter[] = [{field: 'size_bytes', op: '>', value: 100}];
    ds.useRows(
      flatModel({limit: 100, columns: ['file_name', 'size_bytes'], filters}),
    );
    await flush();

    expect(calls).toHaveLength(1);
    expect(calls[0].columns).toEqual(['file_name', 'size_bytes']);
    // The data source passes the Filter[] straight through; the HTTP client is
    // what coerces values to the always-strings wire form.
    expect(calls[0].filter).toEqual(filters);
  });

  test('omits the projection when no columns are visible', async () => {
    const {client, calls} = fakeClient();
    const ds = new BigtraceTraceListDataSource(client, () => SETTINGS);
    ds.useRows(flatModel({limit: 100}));
    await flush();
    expect(calls[0].columns).toBeUndefined();
  });

  test('refetches when the settings thunk changes', async () => {
    const {client, calls} = fakeClient();
    let settings: SettingFilter[] = SETTINGS;
    const ds = new BigtraceTraceListDataSource(client, () => settings);
    ds.useRows(flatModel({limit: 100}));
    await flush();
    expect(calls).toHaveLength(1);

    settings = [
      {
        settingId: 'trace_directory',
        values: ['/other'],
        category: 'TRACE_ADDRESS',
      },
    ];
    ds.useRows(flatModel({limit: 100}));
    await flush();
    expect(calls).toHaveLength(2);
    expect(calls[1].settings).toBe(settings);
  });

  test('refetches only on TRACE_ADDRESS changes, not query-option / metadata edits', async () => {
    const {client, calls} = fakeClient();
    let settings: SettingFilter[] = [
      {settingId: 'trace_directory', values: ['/d'], category: 'TRACE_ADDRESS'},
      {
        settingId: 'row_limit',
        values: ['100'],
        category: 'BIGTRACE_QUERY_OPTIONS',
      },
    ];
    const ds = new BigtraceTraceListDataSource(client, () => settings);
    ds.useRows(flatModel({limit: 100}));
    await flush();
    expect(calls).toHaveLength(1);

    // Editing a non-source setting (query option) leaves the trace set
    // unchanged — must NOT re-hit /trace_metadata.
    settings = [
      {settingId: 'trace_directory', values: ['/d'], category: 'TRACE_ADDRESS'},
      {
        settingId: 'row_limit',
        values: ['500'],
        category: 'BIGTRACE_QUERY_OPTIONS',
      },
    ];
    ds.useRows(flatModel({limit: 100}));
    await flush();
    expect(calls).toHaveLength(1);

    // Changing the TRACE_ADDRESS source DOES refetch.
    settings = [
      {
        settingId: 'trace_directory',
        values: ['/other'],
        category: 'TRACE_ADDRESS',
      },
      {
        settingId: 'row_limit',
        values: ['500'],
        category: 'BIGTRACE_QUERY_OPTIONS',
      },
    ];
    ds.useRows(flatModel({limit: 100}));
    await flush();
    expect(calls).toHaveLength(2);
  });

  test('a failed fetch clears rows and surfaces the error', async () => {
    const {client} = fakeClient({reject: new Error('status: 400 bad dir')});
    const ds = new BigtraceTraceListDataSource(client, () => SETTINGS);
    ds.useRows(flatModel({limit: 100}));
    await flush();

    expect(ds.getError()).toContain('400');
    const rows = ds.useRows(flatModel({limit: 100}));
    expect(rows.rows).toEqual([]);
    expect(rows.totalRows).toBe(0);
  });

  test('a cancelled fetch is swallowed (no error surfaced)', async () => {
    const {client} = fakeClient({reject: new QueryCancelledError()});
    const ds = new BigtraceTraceListDataSource(client, () => SETTINGS);
    ds.useRows(flatModel({limit: 100}));
    await flush();
    expect(ds.getError()).toBeNull();
  });

  test('reports sort changes to onOrderByChange, not the initial fetch', async () => {
    const {client} = fakeClient();
    const seen: string[] = [];
    const ds = new BigtraceTraceListDataSource(
      client,
      () => SETTINGS,
      undefined,
      (o) => seen.push(o),
    );
    // Initial fetch carries no sort → no callback.
    ds.useRows(flatModel({limit: 100}));
    await flush();
    expect(seen).toEqual([]);

    // Sorting reports the AIP-132 order string.
    ds.useRows(
      flatModel({limit: 100, sort: {alias: 'size_bytes', direction: 'DESC'}}),
    );
    await flush();
    expect(seen).toEqual(['size_bytes desc']);

    // Clearing the sort reports the empty string (back to backend default).
    ds.useRows(flatModel({limit: 100}));
    await flush();
    expect(seen).toEqual(['size_bytes desc', '']);
  });

  test('coalesces settings changes during a slow in-flight fetch (only the latest refetches)', async () => {
    // A client whose fetches stay pending until released — simulates a slow
    // backend so we can change settings while a fetch is in flight.
    const calls: ReadonlyArray<SettingFilter>[] = [];
    let resolvers: Array<() => void> = [];
    const client = {
      listTraceMetadata: vi.fn((settings: ReadonlyArray<SettingFilter>) => {
        calls.push(settings);
        return new Promise<QueryResultPage>((resolve) => {
          resolvers.push(() => resolve(PAGE));
        });
      }),
    } as unknown as BigtraceQueryClient;
    const releaseAll = () => {
      const rs = resolvers;
      resolvers = [];
      rs.forEach((r) => r());
    };

    const S1 = SETTINGS;
    const S2: SettingFilter[] = [
      {settingId: 'trace_directory', values: ['/a'], category: 'TRACE_ADDRESS'},
    ];
    const S3: SettingFilter[] = [
      {
        settingId: 'trace_directory',
        values: ['/abc'],
        category: 'TRACE_ADDRESS',
      },
    ];
    let settings: SettingFilter[] = S1;
    const ds = new BigtraceTraceListDataSource(client, () => settings);

    // First render starts a fetch for S1 that never resolves (stays in flight).
    ds.useRows(flatModel({limit: 100}));
    await flush();
    expect(calls).toHaveLength(1);

    // Two settings changes while the fetch is in flight — both skipped by the
    // !isFetching guard, so no new requests fire.
    settings = S2;
    ds.useRows(flatModel({limit: 100}));
    await flush();
    settings = S3;
    ds.useRows(flatModel({limit: 100}));
    await flush();
    expect(calls).toHaveLength(1);

    // The slow fetch completes; the redraw-driven next render fetches ONLY the
    // latest settings (S3) — the intermediate S2 was coalesced away.
    releaseAll();
    await flush();
    ds.useRows(flatModel({limit: 100}));
    await flush();
    expect(calls).toHaveLength(2);
    expect(calls[1]).toBe(S3);
  });
});
