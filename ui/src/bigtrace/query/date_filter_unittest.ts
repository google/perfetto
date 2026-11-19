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
import {
  dateFilterToFilters,
  describeDateFilter,
  extractDateFilter,
  splitDateFilters,
} from './date_filter';

const TODAY = '2026-10-05';

describe('date filter', () => {
  test('relative range resolves to a >= / <= pair ending today', () => {
    expect(dateFilterToFilters({kind: 'last', days: 7}, TODAY)).toEqual([
      {field: 'upload_date', op: '>=', value: '2026-09-29'},
      {field: 'upload_date', op: '<=', value: '2026-10-05'},
    ]);
    expect(dateFilterToFilters(undefined, TODAY)).toEqual([]);
    expect(dateFilterToFilters(null, TODAY)).toEqual([]);
    expect(
      dateFilterToFilters({kind: 'fixed', from: '2026-09-20'}, TODAY),
    ).toEqual([{field: 'upload_date', op: '>=', value: '2026-09-20'}]);
  });

  test('chip labels', () => {
    expect(describeDateFilter({kind: 'last', days: 7}, TODAY)).toBe(
      'Last 7 days (Sep 29 – Oct 5)',
    );
    expect(
      describeDateFilter(
        {kind: 'fixed', from: '2026-09-20', to: '2026-09-25'},
        TODAY,
      ),
    ).toBe('Sep 20 – Sep 25');
    expect(describeDateFilter({kind: 'fixed', to: '2026-09-25'}, TODAY)).toBe(
      'Until Sep 25',
    );
    expect(describeDateFilter(null, TODAY)).toBe('Upload date: Any');
  });

  test('removing half of the pair in the grid leaves the range', () => {
    const pair = dateFilterToFilters({kind: 'last', days: 7}, TODAY);
    const own = {
      field: 'device_manufacturer',
      op: 'in',
      value: ['Google'],
    } as const;
    expect(splitDateFilters([...pair, own], pair)).toEqual({
      rest: [own],
      intact: true,
    });
    expect(splitDateFilters([pair[0]!, own], pair)).toEqual({
      rest: [own],
      intact: false,
    });
    // A grid filter that duplicates one half is swept out with the pair but
    // is not a removal: the range stays.
    expect(splitDateFilters([pair[0]!, pair[0]!, pair[1]!, own], pair)).toEqual(
      {
        rest: [own],
        intact: true,
      },
    );
  });

  test("a history run's date pair becomes a fixed range", () => {
    const own = {
      field: 'device_manufacturer',
      op: 'in',
      value: ['Google'],
    } as const;
    const pair = dateFilterToFilters({kind: 'last', days: 7}, TODAY);
    expect(extractDateFilter([...pair, own])).toEqual({
      rest: [own],
      dateFilter: {kind: 'fixed', from: '2026-09-29', to: '2026-10-05'},
    });
    expect(extractDateFilter([own]).dateFilter).toBeUndefined();
  });
});
