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

import {afterEach, describe, expect, test, vi} from 'vitest';
import {
  BigtraceHttpError,
  BigtraceQueryClient,
  parseQueryResponse,
  QueryNotFoundError,
} from './bigtrace_query_client';
import {coerceFiltersForWire, encodeFilters} from './filter_encoding';
import type {Filter} from '../../components/widgets/datagrid/model';
import type {SettingFilter} from '../settings/settings_types';

// Both the native-array body and the change-detection-key string route through
// `coerceFiltersForWire`; lock the always-strings coercion in once here.
describe('coerceFiltersForWire', () => {
  test('passes strings and JSON null through unchanged', () => {
    expect(
      coerceFiltersForWire([
        {field: 'name', op: 'glob', value: 'ui::*'},
        {field: 'kind', op: 'in', value: ['a', 'b']},
        {field: 'parent_id', op: 'is null'},
      ]),
    ).toEqual([
      {field: 'name', op: 'glob', value: 'ui::*'},
      {field: 'kind', op: 'in', value: ['a', 'b']},
      {field: 'parent_id', op: 'is null'},
    ]);
  });

  test('coerces numbers / bigints / booleans to strings (always-strings wire)', () => {
    // int64 past Number.MAX_SAFE_INTEGER survives as a string (no precision
    // loss).
    expect(
      coerceFiltersForWire([
        {field: 'count', op: '>=', value: 10},
        {field: 'dur', op: '>', value: 9223372036854775807n},
        JSON.parse('{"field":"flag","op":"=","value":true}'),
      ]),
    ).toEqual([
      {field: 'count', op: '>=', value: '10'},
      {field: 'dur', op: '>', value: '9223372036854775807'},
      {field: 'flag', op: '=', value: 'true'},
    ]);
  });

  test('coerces every element of an in-list', () => {
    expect(
      coerceFiltersForWire([{field: 'tid', op: 'in', value: [1n, 2, 3n]}]),
    ).toEqual([{field: 'tid', op: 'in', value: ['1', '2', '3']}]);
  });

  test('preserves JSON null distinct from the literal "null" string', () => {
    // null must NOT collapse to the literal "null" text value.
    expect(coerceFiltersForWire([{field: 'a', op: '=', value: null}])).toEqual([
      {field: 'a', op: '=', value: null},
    ]);
  });

  test('returns a fresh array (does not mutate the input)', () => {
    const input: Filter[] = [{field: 'n', op: '=', value: 1}];
    const out = coerceFiltersForWire(input);
    expect(out).not.toBe(input);
    expect(input[0]).toEqual({field: 'n', op: '=', value: 1});
  });
});

// The change-detection key must be canonical (key-sorted) so equivalent
// filters compare equal.
describe('encodeFilters', () => {
  test('shares value coercion with coerceFiltersForWire', () => {
    expect(
      JSON.parse(
        encodeFilters([{field: 'dur', op: '>', value: 9007199254740993n}]),
      ),
    ).toEqual([{field: 'dur', op: '>', value: '9007199254740993'}]);
  });

  test('produces canonical (key-sorted) output regardless of construction order', () => {
    const a = encodeFilters([{field: 'x', op: '=', value: '1'}]);
    const b = encodeFilters([JSON.parse('{"value":"1","op":"=","field":"x"}')]);
    expect(a).toBe(b);
    expect(a).toBe('[{"field":"x","op":"=","value":"1"}]');
  });

  test('empty array → "[]"', () => {
    expect(encodeFilters([])).toBe('[]');
  });
});

describe('BigtraceQueryClient trace-metadata wire', () => {
  const originalFetch = global.fetch;
  afterEach(() => {
    global.fetch = originalFetch;
  });

  function captureFetch(payload: unknown = {}): ReturnType<typeof vi.fn> {
    const body = JSON.stringify(payload);
    const fakeResp = {
      ok: true,
      status: 200,
      text: () => Promise.resolve(body),
      json: () => Promise.resolve(payload),
    };
    const fn = vi.fn().mockResolvedValue(fakeResp);
    global.fetch = fn as unknown as typeof fetch;
    return fn;
  }

  function urlFrom(fetchMock: ReturnType<typeof vi.fn>): string {
    return (fetchMock.mock.calls[0] as unknown[])[0] as string;
  }

  function initFrom(fetchMock: ReturnType<typeof vi.fn>): RequestInit {
    return (fetchMock.mock.calls[0] as unknown[])[1] as RequestInit;
  }

  function bodyFrom(
    fetchMock: ReturnType<typeof vi.fn>,
  ): Record<string, unknown> {
    return JSON.parse(initFrom(fetchMock).body as string);
  }

  const traceDir: SettingFilter = {
    settingId: 'trace_directory',
    values: ['/var/traces'],
    category: 'TRACE_ADDRESS',
  };

  test('listTraceMetadata POSTs /trace_metadata with mapped settings + paging', async () => {
    const fetchMock = captureFetch({columnNames: ['file_name'], rows: []});
    const client = new BigtraceQueryClient('http://example/');
    await client.listTraceMetadata([traceDir], 50, 10);

    expect(urlFrom(fetchMock)).toBe('http://example//trace_metadata');
    expect(initFrom(fetchMock).method).toBe('POST');
    const body = bodyFrom(fetchMock);
    // Settings map from camelCase to the snake_case wire fields.
    expect(body.settings).toEqual([
      {
        setting_id: 'trace_directory',
        values: ['/var/traces'],
        category: 'TRACE_ADDRESS',
      },
    ]);
    expect(body.limit).toBe(50);
    expect(body.offset).toBe(10);
    // Optional fields stay off the wire when not provided.
    expect(body.order_by).toBeUndefined();
    expect(body.filters).toBeUndefined();
    expect(body.columns).toBeUndefined();
  });

  test('listTraceMetadata parses the always-strings response into rows', async () => {
    const fetchMock = captureFetch({
      columnNames: ['file_name', 'size_bytes'],
      rows: [{values: ['a.pftrace', '9007199254740993']}],
    });
    const client = new BigtraceQueryClient('http://example/');
    const page = await client.listTraceMetadata([traceDir], 50, 0);
    expect(page.columns).toEqual(['file_name', 'size_bytes']);
    // Big values survive as strings (no Number coercion, no precision loss).
    expect(page.rows).toEqual([
      {file_name: 'a.pftrace', size_bytes: '9007199254740993'},
    ]);
    expect(urlFrom(fetchMock)).toContain('/trace_metadata');
  });

  test('listTraceMetadata ships order_by, native-array filters, and columns when set', async () => {
    const fetchMock = captureFetch({columnNames: [], rows: []});
    const client = new BigtraceQueryClient('http://example/');
    await client.listTraceMetadata(
      [],
      50,
      0,
      undefined,
      'size_bytes desc',
      [{field: 'size_bytes', op: '>', value: 100}],
      ['file_name', 'size_bytes'],
    );
    const body = bodyFrom(fetchMock);
    expect(body.order_by).toBe('size_bytes desc');
    // Native JSON array, not a stringified blob.
    expect(Array.isArray(body.filters)).toBe(true);
    expect(body.filters).toEqual([
      {field: 'size_bytes', op: '>', value: '100'},
    ]);
    expect(body.columns).toEqual(['file_name', 'size_bytes']);
  });

  test('listTraceMetadata omits empty order_by / filter / columns', async () => {
    const fetchMock = captureFetch({columnNames: [], rows: []});
    const client = new BigtraceQueryClient('http://example/');
    await client.listTraceMetadata([], 50, 0, undefined, '', [], []);
    const body = bodyFrom(fetchMock);
    expect(body.order_by).toBeUndefined();
    expect(body.filters).toBeUndefined();
    expect(body.columns).toBeUndefined();
  });

  test('listTraceMetadataSchema POSTs /trace_metadata_schema with settings', async () => {
    const fetchMock = captureFetch({
      columns: [
        {name: 'file_name', type: 'TEXT', defaultVisible: true},
        {name: 'size_bytes', type: 'BIGINT', defaultVisible: false},
      ],
    });
    const client = new BigtraceQueryClient('http://example/');
    const schema = await client.listTraceMetadataSchema([traceDir]);

    expect(urlFrom(fetchMock)).toBe('http://example//trace_metadata_schema');
    expect(initFrom(fetchMock).method).toBe('POST');
    expect(bodyFrom(fetchMock).settings).toEqual([
      {
        setting_id: 'trace_directory',
        values: ['/var/traces'],
        category: 'TRACE_ADDRESS',
      },
    ]);
    expect(schema.columns.map((c) => c.name)).toEqual([
      'file_name',
      'size_bytes',
    ]);
    expect(schema.columns[0].defaultVisible).toBe(true);
  });
});

describe('BigtraceQueryClient execute trace-selection snapshot', () => {
  const originalFetch = global.fetch;
  afterEach(() => {
    global.fetch = originalFetch;
  });

  function captureFetch(): ReturnType<typeof vi.fn> {
    const payload = {queryUuid: 'uid', columnNames: [], rows: []};
    const fakeResp = {
      ok: true,
      status: 200,
      text: () => Promise.resolve(JSON.stringify(payload)),
      json: () => Promise.resolve(payload),
    };
    const fn = vi.fn().mockResolvedValue(fakeResp);
    global.fetch = fn as unknown as typeof fetch;
    return fn;
  }

  function bodyFrom(
    fetchMock: ReturnType<typeof vi.fn>,
  ): Record<string, unknown> {
    const init = (fetchMock.mock.calls[0] as unknown[])[1] as RequestInit;
    return JSON.parse(init.body as string);
  }

  test('omits every trace_* field when no options are passed', async () => {
    const fetchMock = captureFetch();
    const client = new BigtraceQueryClient('http://example/');
    await client.executeAsync('select 1', 100, []);
    const body = bodyFrom(fetchMock);
    expect(body.perfetto_sql).toBe('select 1');
    expect(body.limit).toBe(100);
    expect(body.trace_filters).toBeUndefined();
    expect(body.trace_metadata_columns).toBeUndefined();
    expect(body.trace_order_by).toBeUndefined();
    expect(body.trace_limit).toBeUndefined();
  });

  test('ships trace_filters as a native, coerced array', async () => {
    const fetchMock = captureFetch();
    const client = new BigtraceQueryClient('http://example/');
    await client.executeAsync('select 1', 100, [], undefined, {
      traceFilters: [
        {field: 'file_name', op: 'glob', value: '*.pftrace'},
        {field: 'size_bytes', op: '>', value: 1000},
      ],
    });
    const body = bodyFrom(fetchMock);
    expect(Array.isArray(body.trace_filters)).toBe(true);
    expect(body.trace_filters).toEqual([
      {field: 'file_name', op: 'glob', value: '*.pftrace'},
      {field: 'size_bytes', op: '>', value: '1000'},
    ]);
  });

  test('ships trace_metadata_columns / trace_order_by / trace_limit when set', async () => {
    const fetchMock = captureFetch();
    const client = new BigtraceQueryClient('http://example/');
    await client.executeSync('select 1', 100, [], undefined, {
      traceMetadataColumns: ['device_name', 'android_id'],
      traceOrderBy: 'size_bytes desc',
      traceLimit: 50,
    });
    const body = bodyFrom(fetchMock);
    expect(body.trace_metadata_columns).toEqual(['device_name', 'android_id']);
    expect(body.trace_order_by).toBe('size_bytes desc');
    expect(body.trace_limit).toBe(50);
  });

  test('omits empty / default trace_* fields', async () => {
    const fetchMock = captureFetch();
    const client = new BigtraceQueryClient('http://example/');
    await client.executeAsync('select 1', 100, [], undefined, {
      traceFilters: [],
      traceMetadataColumns: [],
      traceOrderBy: '',
      traceLimit: 0,
    });
    const body = bodyFrom(fetchMock);
    expect(body.trace_filters).toBeUndefined();
    expect(body.trace_metadata_columns).toBeUndefined();
    expect(body.trace_order_by).toBeUndefined();
    expect(body.trace_limit).toBeUndefined();
  });
});

describe('BigtraceQueryClient.fetchResults', () => {
  const originalFetch = global.fetch;
  afterEach(() => {
    global.fetch = originalFetch;
  });

  function captureFetch(
    payload: unknown = {columnNames: [], rows: []},
  ): ReturnType<typeof vi.fn> {
    const body = JSON.stringify(payload);
    const fakeResp = {
      ok: true,
      status: 200,
      text: () => Promise.resolve(body),
      json: () => Promise.resolve(payload),
    };
    const fn = vi.fn().mockResolvedValue(fakeResp);
    global.fetch = fn as unknown as typeof fetch;
    return fn;
  }

  function urlFrom(fetchMock: ReturnType<typeof vi.fn>): string {
    return (fetchMock.mock.calls[0] as unknown[])[0] as string;
  }

  function initFrom(fetchMock: ReturnType<typeof vi.fn>): RequestInit {
    return (fetchMock.mock.calls[0] as unknown[])[1] as RequestInit;
  }

  function bodyFrom(
    fetchMock: ReturnType<typeof vi.fn>,
  ): Record<string, unknown> {
    return JSON.parse(initFrom(fetchMock).body as string);
  }

  test('POSTs to a plain :fetch_results URL with limit/offset in the body', async () => {
    const fetchMock = captureFetch();
    const client = new BigtraceQueryClient('http://example/');
    await client.fetchResults('uid', 50, 10);
    expect(urlFrom(fetchMock)).toBe(
      'http://example//query_executions/uid:fetch_results',
    );
    expect(initFrom(fetchMock).method).toBe('POST');
    expect(urlFrom(fetchMock)).not.toContain('?');
    const body = bodyFrom(fetchMock);
    expect(body.limit).toBe(50);
    expect(body.offset).toBe(10);
    expect(body.order_by).toBeUndefined();
    expect(body.filters).toBeUndefined();
    expect(body.columns).toBeUndefined();
  });

  test('ships order_by, native-array filters, and the columns field-mask', async () => {
    const fetchMock = captureFetch();
    const client = new BigtraceQueryClient('http://example/');
    await client.fetchResults(
      'uid',
      50,
      0,
      undefined,
      'dur desc',
      [{field: 'dur', op: '>', value: 9223372036854775807n}],
      ['name', 'dur', 'device_name'],
    );
    const body = bodyFrom(fetchMock);
    expect(body.order_by).toBe('dur desc');
    expect(Array.isArray(body.filters)).toBe(true);
    // bigint coerced to string for lossless int64 round-trip.
    expect(body.filters).toEqual([
      {field: 'dur', op: '>', value: '9223372036854775807'},
    ]);
    expect(body.columns).toEqual(['name', 'dur', 'device_name']);
  });

  test('exposes availableColumnNames from the response (the column-picker union)', async () => {
    const fetchMock = captureFetch({
      columnNames: ['name', 'dur'],
      rows: [{values: ['slice', '10']}],
      availableColumnNames: ['name', 'dur', 'device_name', 'android_id'],
    });
    const client = new BigtraceQueryClient('http://example/');
    const page = await client.fetchResults('uid', 50, 0);
    expect(page.columns).toEqual(['name', 'dur']);
    expect(page.availableColumnNames).toEqual([
      'name',
      'dur',
      'device_name',
      'android_id',
    ]);
    expect(urlFrom(fetchMock)).toContain(':fetch_results');
  });
});

describe('BigtraceQueryClient error responses', () => {
  const originalFetch = global.fetch;
  afterEach(() => {
    global.fetch = originalFetch;
  });

  function failWith(status: number, body: string): void {
    const fakeResp = {
      ok: false,
      status,
      text: () => Promise.resolve(body),
      json: () => Promise.reject(new Error('not called on error path')),
    };
    global.fetch = vi
      .fn()
      .mockResolvedValue(fakeResp) as unknown as typeof fetch;
  }

  test('a 400 throws BigtraceHttpError carrying the backend detail and status', async () => {
    failWith(400, JSON.stringify({detail: "unknown column 'foo'"}));
    const client = new BigtraceQueryClient('http://example/');
    const err = await client.fetchResults('uid', 10, 0).catch((e) => e);
    expect(err).toBeInstanceOf(BigtraceHttpError);
    expect(err.status).toBe(400);
    expect(err.detail).toBe("unknown column 'foo'");
  });

  test('a non-JSON error body becomes the detail verbatim', async () => {
    failWith(500, 'upstream exploded');
    const client = new BigtraceQueryClient('http://example/');
    const err = await client.fetchResults('uid', 10, 0).catch((e) => e);
    expect(err).toBeInstanceOf(BigtraceHttpError);
    expect(err.status).toBe(500);
    expect(err.detail).toBe('upstream exploded');
  });

  test('a 404 throws QueryNotFoundError, not BigtraceHttpError', async () => {
    failWith(404, JSON.stringify({detail: 'gone'}));
    const client = new BigtraceQueryClient('http://example/');
    const err = await client.fetchResults('uid', 10, 0).catch((e) => e);
    expect(err).toBeInstanceOf(QueryNotFoundError);
  });
});

describe('parseQueryResponse', () => {
  test('passes values through: JSON null is SQL NULL, the string "NULL" stays text', () => {
    const page = parseQueryResponse({
      columnNames: ['name', 'note'],
      rows: [{values: ['NULL', null]}, {values: ['x', 'y']}],
    });
    expect(page.columns).toEqual(['name', 'note']);
    expect(page.rows[0]).toEqual({name: 'NULL', note: null});
    expect(page.rows[1]).toEqual({name: 'x', note: 'y'});
  });
});
