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
import {addDays, formatLongDate, shiftMonth, yearMonthOf} from './iso_date';

describe('iso_date', () => {
  test('addDays crosses month ends', () => {
    expect(addDays('2026-10-05', -6)).toBe('2026-09-29');
  });

  test('formatLongDate spells the whole day out', () => {
    expect(formatLongDate('2026-10-05')).toBe('October 5, 2026');
    expect(formatLongDate('2028-02-29')).toBe('February 29, 2028');
  });

  test('shiftMonth wraps years', () => {
    expect(shiftMonth(yearMonthOf('2026-01-15'), -1)).toEqual({
      year: 2025,
      month: 11,
    });
  });
});
