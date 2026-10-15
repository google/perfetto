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

import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {Time} from '../../base/time';
import type {QueryResult} from '../../trace_processor/query_result';
import {BusyTime, LiveMinimapProvider} from './minimap';
import {SLICE_TABLE} from './store';
import {queryResult} from './test_utils';

// The bucket width of BusyTime.
const B = 1n << 30n;

// The minimap's span: 10 buckets.
const SPAN = HighPrecisionTimeSpan.fromTime(Time.ZERO, Time.fromRaw(10n * B));

// Answers queries with `result`, and records them. Without a result, queries
// fail.
function fakeEngine(result?: QueryResult) {
  const queries: string[] = [];
  return {
    queries,
    query: async (sql: string) => {
      queries.push(sql);
      if (result === undefined) throw new Error('Unexpected query');
      return result;
    },
  };
}

test('sums the busy time of each track by cell', () => {
  const busy = new BusyTime();
  busy.add(2, 0n, B / 2n);
  busy.add(1, B, 2n * B);
  busy.add(1, B + 5n, B / 4n);
  // Instants, and slices outside the span.
  busy.add(1, 0n, 0n);
  busy.add(1, 20n * B, B);

  // Relative to the cell width, at most 1; by track id.
  expect(busy.rows(0n, 10n * B, B)).toEqual([
    [{ts: B, dur: B, load: 1}],
    [{ts: 0n, dur: B, load: 0.5}],
  ]);
  expect(busy.rows(0n, 10n * B, 4n * B)).toEqual([
    [{ts: 0n, dur: 4n * B, load: 0.5625}],
    [{ts: 0n, dur: 4n * B, load: 0.125}],
  ]);
});

test('forgets the slices that start before the bucket of a cutoff', () => {
  const busy = new BusyTime();
  busy.add(1, 0n, B);
  busy.add(1, B, B);
  busy.add(2, 0n, B);

  // Within bucket 1.
  busy.prune(B + 5n);

  expect(busy.rows(0n, 10n * B, B)).toEqual([[{ts: B, dur: B, load: 1}]]);
});

test('starts the cells at the start of the span', () => {
  const busy = new BusyTime();
  busy.add(1, 2n * B, B);
  busy.add(1, 3n * B, B);

  // Buckets 2 and 3 start 0.75 and 1.25 cells in.
  expect(busy.rows(B / 2n, 10n * B, 2n * B)).toEqual([
    [
      {ts: B / 2n, dur: 2n * B, load: 0.5},
      {ts: B / 2n + 2n * B, dur: 2n * B, load: 0.5},
    ],
  ]);
});

test('loads the busy time of the stored slices', async () => {
  const engine = fakeEngine(
    queryResult(
      ['trackId', 'bucket', 'busy'],
      [
        [1, 2, Number(B)],
        [3, 2, Number(B / 4n)],
      ],
    ),
  );
  const busy = new BusyTime();
  busy.add(9, 0n, B);

  await busy.load(engine);

  // Replaces the sums.
  expect(engine.queries[0]).toContain(`FROM ${SLICE_TABLE}`);
  expect(busy.rows(0n, 10n * B, B)).toEqual([
    [{ts: 2n * B, dur: B, load: 1}],
    [{ts: 2n * B, dur: B, load: 0.25}],
  ]);
});

test('shows the busy time in cells of at least a bucket', async () => {
  const engine = fakeEngine();
  const busy = new BusyTime();
  busy.add(1, B, B);

  const provider = new LiveMinimapProvider(engine, busy);

  expect(await provider.getData(SPAN, 2n * B)).toEqual([
    [{ts: 0n, dur: 2n * B, load: 0.5}],
  ]);
  expect(engine.queries).toEqual([]);
});

test('queries the slices for smaller cells', async () => {
  const engine = fakeEngine(
    queryResult(
      ['trackId', 'cell', 'load'],
      [
        [1, 0, 0.5],
        [1, 3, 1],
        [2, 1, 0.25],
      ],
    ),
  );
  const res = B / 4n;

  const provider = new LiveMinimapProvider(engine, new BusyTime());

  expect(await provider.getData(SPAN, res)).toEqual([
    [
      {ts: 0n, dur: res, load: 0.5},
      {ts: 3n * res, dur: res, load: 1},
    ],
    [{ts: res, dur: res, load: 0.25}],
  ]);
  expect(engine.queries[0]).toContain(`FROM ${SLICE_TABLE}`);
});

test('needs a resolution', async () => {
  const busy = new BusyTime();
  busy.add(1, 0n, B);

  const provider = new LiveMinimapProvider(fakeEngine(), busy);

  expect(await provider.getData(SPAN, 0n)).toEqual([]);
});
