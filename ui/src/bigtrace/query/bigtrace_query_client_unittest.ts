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
  BigtraceQueryClient,
  QueryNotFoundError,
  parseQueryResponse,
} from './bigtrace_query_client';
import {encodeFilters} from './filter_encoding';

describe('parseQueryResponse', () => {
  test('returns empty result for null/undefined response', () => {
    expect(parseQueryResponse({})).toEqual({rows: [], columns: []});
    expect(parseQueryResponse({columnNames: ['a'], rows: undefined})).toEqual({
      rows: [],
      columns: [],
    });
  });

  test('aligns row values with column names', () => {
    const result = parseQueryResponse({
      columnNames: ['a', 'b', 'c'],
      rows: [{values: [1, 'two', null]}],
    });
    expect(result.columns).toEqual(['a', 'b', 'c']);
    expect(result.rows).toEqual([{a: 1, b: 'two', c: null}]);
  });

  test('preserves number values without round-tripping through Number()', () => {
    const result = parseQueryResponse({
      columnNames: ['ts'],
      rows: [{values: [1_000_000_000]}],
    });
    expect(result.rows[0].ts).toBe(1_000_000_000);
    expect(typeof result.rows[0].ts).toBe('number');
  });

  test('preserves numeric strings as strings (no precision loss)', () => {
    // The backend may serialize a 64-bit int as a string to preserve
    // precision; we must not silently widen it to JS Number.
    const big = '9007199254740993'; // 2^53 + 1, not representable as Number.
    const result = parseQueryResponse({
      columnNames: ['id'],
      rows: [{values: [big]}],
    });
    expect(result.rows[0].id).toBe(big);
    expect(typeof result.rows[0].id).toBe('string');
  });

  test('preserves empty strings (does not coerce to 0)', () => {
    const result = parseQueryResponse({
      columnNames: ['s'],
      rows: [{values: ['']}],
    });
    expect(result.rows[0].s).toBe('');
  });

  test("translates the 'NULL' SQL marker into JS null", () => {
    const result = parseQueryResponse({
      columnNames: ['x'],
      rows: [{values: ['NULL']}],
    });
    expect(result.rows[0].x).toBeNull();
  });

  test('preserves explicit JSON null', () => {
    const result = parseQueryResponse({
      columnNames: ['x'],
      rows: [{values: [null]}],
    });
    expect(result.rows[0].x).toBeNull();
  });

  test('multiple rows in the right order', () => {
    const result = parseQueryResponse({
      columnNames: ['n'],
      rows: [{values: [1]}, {values: [2]}, {values: [3]}],
    });
    expect(result.rows.map((r) => r.n)).toEqual([1, 2, 3]);
  });

  test('passes through totalFilteredRows when present', () => {
    const result = parseQueryResponse({
      columnNames: ['n'],
      rows: [{values: [1]}],
      totalFilteredRows: 42,
    });
    expect(result.totalFilteredRows).toBe(42);
  });

  test('totalFilteredRows is undefined when absent (legacy backends)', () => {
    const result = parseQueryResponse({
      columnNames: ['n'],
      rows: [{values: [1]}],
    });
    expect(result.totalFilteredRows).toBeUndefined();
  });
});

describe('encodeFilters', () => {
  test('strings and null pass through unchanged', () => {
    const out = encodeFilters([
      {field: 'name', op: 'glob', value: 'ui::*'},
      {field: 'kind', op: 'in', value: ['a', 'b']},
      {field: 'parent_id', op: 'is null'},
    ]);
    expect(JSON.parse(out)).toEqual([
      {field: 'name', op: 'glob', value: 'ui::*'},
      {field: 'kind', op: 'in', value: ['a', 'b']},
      {field: 'parent_id', op: 'is null'},
    ]);
  });

  test('coerces non-string primitives so the wire is always-strings', () => {
    // The wire spec is `value: string`. DuckDB does the column-typed
    // coercion at bind time, so we don't need to preserve numeric
    // identity on the wire — and stringifying every primitive means
    // int64 values past Number.MAX_SAFE_INTEGER round-trip without
    // precision loss (`bigint.toString()` is lossless). Booleans
    // aren't part of `SqlValue`, but the encoder handles them too in
    // case a future widget contract widens the value type — tested
    // via JSON.parse to bypass TS's structural check.
    const out = encodeFilters([
      {field: 'count', op: '>=', value: 10},
      {field: 'dur', op: '>', value: 9223372036854775807n},
      JSON.parse('{"field":"flag","op":"=","value":true}'),
    ]);
    expect(JSON.parse(out)).toEqual([
      {field: 'count', op: '>=', value: '10'},
      {field: 'dur', op: '>', value: '9223372036854775807'},
      {field: 'flag', op: '=', value: 'true'},
    ]);
  });

  test('coerces every entry of an in-list', () => {
    const out = encodeFilters([{field: 'tid', op: 'in', value: [1n, 2, 3n]}]);
    expect(JSON.parse(out)).toEqual([
      {field: 'tid', op: 'in', value: ['1', '2', '3']},
    ]);
  });

  test('preserves JSON null distinct from the literal "null" string', () => {
    // Filtering `col = null` is normally meaningless in SQL (always
    // false; users want `is null`), but if a client deliberately
    // sends null we round-trip it as JSON null rather than coercing
    // to the string "null", which would match VARCHAR cells with
    // that literal text.
    const out = encodeFilters([{field: 'a', op: '=', value: null}]);
    expect(out).toBe('[{"field":"a","op":"=","value":null}]');
  });

  test('empty array → "[]"', () => {
    expect(encodeFilters([])).toBe('[]');
  });

  test('produces canonical (key-sorted) output for stable equality', () => {
    // Two semantically-equal filters with different property
    // construction orders must serialize to the SAME string —
    // otherwise the data source's `currentFilterKey` equality test
    // would trigger spurious refetches whenever the filter object
    // happened to be built differently. Build via JSON.parse so the
    // engine actually preserves a non-natural insertion order.
    const a = encodeFilters([{field: 'x', op: '=', value: '1'}]);
    const b = encodeFilters([JSON.parse('{"value":"1","op":"=","field":"x"}')]);
    expect(a).toBe(b);
    // And the canonical form is alphabetical.
    expect(a).toBe('[{"field":"x","op":"=","value":"1"}]');
  });
});

describe('BigtraceQueryClient.fetchResults URL construction', () => {
  // We only assert the URL the client builds — the response handling
  // is covered by parseQueryResponse tests. fakeResponse mirrors the
  // 404 tests below.
  const originalFetch = global.fetch;
  afterEach(() => {
    global.fetch = originalFetch;
  });

  function captureFetch(): jest.Mock {
    const fakeResp = {
      ok: true,
      status: 200,
      text: () => Promise.resolve(JSON.stringify({columnNames: [], rows: []})),
      json: () => Promise.resolve({columnNames: [], rows: []}),
    };
    const fn = jest.fn().mockResolvedValue(fakeResp);
    global.fetch = fn as unknown as typeof fetch;
    return fn;
  }

  test('omits filter param when none / empty array passed', async () => {
    const fetchMock = captureFetch();
    const client = new BigtraceQueryClient('http://example/');
    await client.fetchResults('uid', 50, 0);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    const url = (fetchMock.mock.calls[0] as unknown[])[0] as string;
    expect(url).not.toContain('filter=');

    fetchMock.mockClear();
    await client.fetchResults('uid', 50, 0, undefined, undefined, []);
    const url2 = (fetchMock.mock.calls[0] as unknown[])[0] as string;
    expect(url2).not.toContain('filter=');
  });

  test('URL-encodes the JSON-encoded filter payload', async () => {
    const fetchMock = captureFetch();
    const client = new BigtraceQueryClient('http://example/');
    await client.fetchResults('uid', 50, 0, undefined, undefined, [
      {field: 'name', op: 'glob', value: 'ui::*'},
    ]);
    const url = (fetchMock.mock.calls[0] as unknown[])[0] as string;
    expect(url).toContain('filter=');
    // The literal '*' / ':' / '"' would all be percent-encoded; the
    // exact string is determined by encodeURIComponent + JSON.stringify.
    const want = encodeURIComponent(
      JSON.stringify([{field: 'name', op: 'glob', value: 'ui::*'}]),
    );
    expect(url).toContain(`filter=${want}`);
  });

  test('order_by and filter both appear in the URL when set', async () => {
    const fetchMock = captureFetch();
    const client = new BigtraceQueryClient('http://example/');
    await client.fetchResults('uid', 50, 0, undefined, 'name desc', [
      {field: 'kind', op: '=', value: 'sched'},
    ]);
    const url = (fetchMock.mock.calls[0] as unknown[])[0] as string;
    expect(url).toContain('order_by=name%20desc');
    expect(url).toContain('filter=');
  });
});

describe('BigtraceQueryClient 404 handling', () => {
  // The runner's resume-from-history path drops the persisted UUID when
  // it sees a QueryNotFoundError instead of polling forever. That contract
  // depends on the client converting HTTP 404 into this specific class.
  // Jest runs in a Node env without `Response`, so we hand-roll a minimal
  // response-shaped object — only the fields the client touches.
  const originalFetch = global.fetch;
  afterEach(() => {
    global.fetch = originalFetch;
  });

  function fakeResponse(status: number, body: string): unknown {
    return {
      ok: status >= 200 && status < 300,
      status,
      text: () => Promise.resolve(body),
      json: () => Promise.resolve(JSON.parse(body)),
    };
  }

  function mockStatus(status: number, body: string): void {
    global.fetch = jest.fn().mockResolvedValue(fakeResponse(status, body));
  }

  test('getQueryExecution rejects with QueryNotFoundError on 404', async () => {
    const uuid = 'abc-123';
    mockStatus(404, JSON.stringify({detail: `Query ${uuid} not found`}));
    const client = new BigtraceQueryClient('http://example/');
    await expect(client.getQueryExecution(uuid)).rejects.toBeInstanceOf(
      QueryNotFoundError,
    );
  });

  test('extracted UUID matches the request path', async () => {
    const uuid = 'abc-123';
    mockStatus(404, JSON.stringify({detail: `Query ${uuid} not found`}));
    const client = new BigtraceQueryClient('http://example/');
    let caught: unknown;
    try {
      await client.getStatus(uuid);
    } catch (e) {
      caught = e;
    }
    expect(caught).toBeInstanceOf(QueryNotFoundError);
    expect((caught as Error).message).toContain(uuid);
  });

  test('non-404 errors are not converted to QueryNotFoundError', async () => {
    mockStatus(500, 'boom');
    const client = new BigtraceQueryClient('http://example/');
    await expect(client.getQueryExecution('any')).rejects.not.toBeInstanceOf(
      QueryNotFoundError,
    );
  });
});
