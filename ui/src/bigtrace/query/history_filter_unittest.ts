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
import {groupHistoryByDay} from './history_store';
import type {QueryExecution} from './query_store';

function entry(uuid: string, startTime?: number): QueryExecution {
  return {
    uuid,
    status: 'SUCCESS',
    processedRows: 0,
    processedTraces: 0,
    totalTraces: 0,
    startTime,
  };
}

describe('groupHistoryByDay', () => {
  test('buckets entries by local calendar day with relative + full date labels', () => {
    const now = new Date(2026, 8, 22, 14, 30);
    const entries = [
      entry('q1', new Date(2026, 8, 22, 11, 15).getTime()),
      entry('q2', new Date(2026, 8, 22, 9, 5).getTime()),
      entry('q3', new Date(2026, 8, 21, 18, 1).getTime()),
      entry('q4', new Date(2026, 8, 18, 10, 0).getTime()),
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
