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

import {describe, expect, test} from 'vitest';
import {ALL_KINDS, filterHistory, groupHistoryByDay} from './history_store';
import type {QueryExecution} from './query_store';

function entry(
  uuid: string,
  materialized?: boolean,
  startTime?: number,
): QueryExecution {
  return {
    uuid,
    status: 'SUCCESS',
    processedRows: 0,
    processedTraces: 0,
    totalTraces: 0,
    materialized,
    startTime,
  } as QueryExecution;
}

const HISTORY = [
  entry('a', true),
  entry('b', false),
  entry('c', true),
  // Pre-snapshot rows can arrive without the field at all.
  entry('d', undefined),
];

const ids = (filter: {ephemeral: boolean; persistent: boolean}) =>
  filterHistory(HISTORY, filter).map((e) => e.uuid);

describe('filterHistory', () => {
  test('both kinds ticked keeps every entry, in order', () => {
    expect(ids(ALL_KINDS)).toEqual(['a', 'b', 'c', 'd']);
  });

  test('persistent only keeps materialized runs', () => {
    expect(ids({ephemeral: false, persistent: true})).toEqual(['a', 'c']);
  });

  test('ephemeral only keeps the rest, including absent flags', () => {
    expect(ids({ephemeral: true, persistent: false})).toEqual(['b', 'd']);
  });

  test('neither ticked shows nothing (the list says so)', () => {
    expect(ids({ephemeral: false, persistent: false})).toEqual([]);
  });

  test('returns a copy, never the input array', () => {
    expect(filterHistory(HISTORY, ALL_KINDS)).not.toBe(HISTORY);
  });
});

describe('groupHistoryByDay', () => {
  test('buckets entries by local calendar day with relative + full date labels', () => {
    const now = new Date(2026, 8, 22, 14, 30);
    const entries = [
      entry('q1', true, new Date(2026, 8, 22, 11, 15).getTime()),
      entry('q2', false, new Date(2026, 8, 22, 9, 5).getTime()),
      entry('q3', true, new Date(2026, 8, 21, 18, 1).getTime()),
      entry('q4', true, new Date(2026, 8, 18, 10, 0).getTime()),
    ];

    expect(
      groupHistoryByDay(entries, now).map((g) => ({
        label: g.label,
        ids: g.entries.map((e) => e.uuid),
      })),
    ).toEqual([
      {label: 'Today', ids: ['q1', 'q2']},
      {label: 'Yesterday', ids: ['q3']},
      {label: 'Sep 18, 2026', ids: ['q4']},
    ]);
  });
});
