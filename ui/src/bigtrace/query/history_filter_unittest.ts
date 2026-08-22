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
import {filterHistory} from './history_store';
import type {QueryExecution} from './query_store';

function entry(uuid: string, materialized?: boolean): QueryExecution {
  return {
    uuid,
    status: 'SUCCESS',
    processedRows: 0,
    processedTraces: 0,
    totalTraces: 0,
    materialized,
  } as QueryExecution;
}

const HISTORY = [
  entry('a', true),
  entry('b', false),
  entry('c', true),
  // Pre-snapshot rows can arrive without the field at all.
  entry('d', undefined),
];

describe('filterHistory', () => {
  test('all keeps every entry, in order', () => {
    expect(filterHistory(HISTORY, 'all').map((e) => e.uuid)).toEqual([
      'a',
      'b',
      'c',
      'd',
    ]);
  });

  test('persistent keeps only materialized runs', () => {
    expect(filterHistory(HISTORY, 'persistent').map((e) => e.uuid)).toEqual([
      'a',
      'c',
    ]);
  });

  test('ephemeral keeps non-materialized runs, including absent flags', () => {
    expect(filterHistory(HISTORY, 'ephemeral').map((e) => e.uuid)).toEqual([
      'b',
      'd',
    ]);
  });

  test('returns a copy, never the input array', () => {
    const all = filterHistory(HISTORY, 'all');
    expect(all).not.toBe(HISTORY);
  });
});
