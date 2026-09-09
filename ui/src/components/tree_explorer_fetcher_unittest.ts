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

import {defer} from '../base/deferred';
import {SharedAsyncDisposable} from '../base/shared_disposable';
import type {Trace} from '../public/trace';
import type {Engine} from '../trace_processor/engine';
import type {QueryResult} from '../trace_processor/query_result';
import type {TreeExplorerState} from '../widgets/tree_explorer';
import {
  TreeExplorerFetcher,
  type TreeExplorerQueryMetric,
} from './tree_explorer_fetcher';

const METRICS: TreeExplorerQueryMetric[] = [
  {
    name: 'Duration',
    unit: 'ns',
    statement: "select 1 as id, null as parentId, 'root' as name, 1 as value",
  },
];
const STATE: TreeExplorerState = {
  selectedMetricId: 'Duration',
  addedMetricIds: [],
  displayMode: 'flamegraph',
  view: {kind: 'TOP_DOWN'},
  filters: [],
};
const RESULT = {
  firstRow: () => ({cumulative_value: 1}),
  iter: () => ({valid: () => false}),
} as unknown as QueryResult;

function createTrace() {
  const query = vi.fn<Engine['query']>().mockResolvedValue(RESULT);
  const tryQuery = vi.fn<Engine['tryQuery']>().mockResolvedValue({
    ok: true,
    value: RESULT,
  });
  const trace = {engine: {query, tryQuery}} as unknown as Trace;
  return {trace, query, tryQuery};
}

test('reuses metric tables until disposal', async () => {
  const {trace, query, tryQuery} = createTrace();
  const fetcher = new TreeExplorerFetcher(trace);
  expect(await fetcher.fetch(METRICS, STATE)).toMatchObject({
    unfilteredCumulativeValue: 1,
  });
  query.mockClear();
  expect(await fetcher.fetch(METRICS, STATE)).toBeDefined();
  expect(query).toHaveBeenCalledTimes(1);
  expect(tryQuery).not.toHaveBeenCalled();
  await fetcher[Symbol.asyncDispose]();
  expect(tryQuery).toHaveBeenCalledExactlyOnceWith(
    expect.stringMatching(/^DROP TABLE IF EXISTS __temp_/),
  );
});

test('ignores fetches after disposal without dependencies', async () => {
  const {trace, query} = createTrace();
  const fetcher = new TreeExplorerFetcher(trace);
  await fetcher[Symbol.asyncDispose]();
  expect(await fetcher.fetch(METRICS, STATE)).toBeUndefined();
  expect(query).not.toHaveBeenCalled();
});

test('does not query a cached table disposed while awaiting the cache lookup', async () => {
  const {trace, query} = createTrace();
  const fetcher = new TreeExplorerFetcher(trace);
  await fetcher.fetch(METRICS, STATE);
  query.mockClear();
  const fetch = fetcher.fetch(METRICS, STATE);
  const disposal = fetcher[Symbol.asyncDispose]();
  expect(await fetch).toBeUndefined();
  await disposal;
  expect(query).not.toHaveBeenCalled();
});

test('ignores fetches while disposal is still dropping tables', async () => {
  const {trace, query, tryQuery} = createTrace();
  const fetcher = new TreeExplorerFetcher(trace);
  await fetcher.fetch(METRICS, STATE);
  query.mockClear();
  const dropped = defer<Awaited<ReturnType<Engine['tryQuery']>>>();
  tryQuery.mockReturnValueOnce(dropped);
  const disposal = fetcher[Symbol.asyncDispose]();
  expect(await fetcher.fetch(METRICS, STATE)).toBeUndefined();
  expect(query).not.toHaveBeenCalled();
  dropped.resolve({ok: true, value: RESULT});
  await disposal;
});

test.each(['creation', 'summary'] as const)(
  'cleans up when disposed during table %s',
  async (phase) => {
    const {trace, query, tryQuery} = createTrace();
    const releaseDependency = vi.fn(async () => {});
    const dependency = SharedAsyncDisposable.wrap({
      [Symbol.asyncDispose]: releaseDependency,
    });
    const fetcher = new TreeExplorerFetcher(trace, [dependency]);
    await dependency[Symbol.asyncDispose]();
    const started = defer<void>();
    const result = defer<QueryResult>();
    if (phase === 'summary') query.mockResolvedValueOnce(RESULT);
    query.mockImplementationOnce(() => {
      started.resolve();
      return result;
    });
    const fetch = fetcher.fetch(METRICS, STATE);
    await started;
    await fetcher[Symbol.asyncDispose]();
    expect(releaseDependency).not.toHaveBeenCalled();
    expect(tryQuery).not.toHaveBeenCalled();
    result.resolve(RESULT);
    expect(await fetch).toBeUndefined();
    expect(query).toHaveBeenCalledTimes(2);
    expect(tryQuery).toHaveBeenCalledTimes(1);
    expect(releaseDependency).toHaveBeenCalledTimes(1);
    expect(tryQuery.mock.invocationCallOrder[0]).toBeLessThan(
      releaseDependency.mock.invocationCallOrder[0],
    );
  },
);

test('cleans up and propagates a query failure', async () => {
  const {trace, query, tryQuery} = createTrace();
  const fetcher = new TreeExplorerFetcher(trace);
  const error = new Error('Invalid query');
  query.mockResolvedValueOnce(RESULT).mockRejectedValueOnce(error);
  await expect(fetcher.fetch(METRICS, STATE)).rejects.toBe(error);
  expect(tryQuery).toHaveBeenCalledTimes(1);
  await fetcher[Symbol.asyncDispose]();
  expect(tryQuery).toHaveBeenCalledTimes(1);
});
