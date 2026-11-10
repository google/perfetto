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

import {afterEach, describe, expect, test} from 'vitest';
import m from 'mithril';
import {pickDate} from './date_range_calendar';
import {Calendar} from './calendar';

describe('DateRangeCalendar', () => {
  test('left calendar picks the start, right picks the end', () => {
    const r = pickDate(
      {from: '2026-09-10', to: '2026-09-20'},
      'start',
      '2026-09-25',
    );
    expect(r).toEqual({from: '2026-09-25', to: undefined});
    expect(pickDate(r, 'end', '2026-09-24')).toEqual(r);
    expect(pickDate(r, 'end', '2026-10-02')).toEqual({
      from: '2026-09-25',
      to: '2026-10-02',
    });
  });
});

describe('Calendar', () => {
  let root: HTMLDivElement | undefined;
  afterEach(() => {
    if (root !== undefined) {
      m.mount(root, null);
      root.remove();
      root = undefined;
    }
  });

  test('day buttons carry the full date for assistive tech', () => {
    root = document.createElement('div');
    document.body.appendChild(root);
    m.mount(root, {
      view: () =>
        m(Calendar, {
          month: {year: 2026, month: 9}, // October 2026 starts on a Thursday.
          onMonthChange: () => {},
          picked: ['2026-10-13'],
          max: '2026-10-13',
          onPick: () => {},
        }),
    });
    const days = [
      ...root.querySelectorAll<HTMLButtonElement>('button.pf-bt-calendar__day'),
    ];
    expect(days).toHaveLength(31);
    expect(root.querySelectorAll('.pf-bt-calendar__day--blank')).toHaveLength(
      3,
    );
    for (const b of days) {
      expect(b.type).toBe('button');
      expect(b.getAttribute('aria-label')).toMatch(/^October \d+, 2026$/);
    }
    const picked = days[12]!;
    expect(picked.getAttribute('aria-label')).toBe('October 13, 2026');
    expect(picked.getAttribute('aria-pressed')).toBe('true');
    expect(picked.disabled).toBe(false);
    expect(days[0]!.getAttribute('aria-pressed')).toBe('false');
    // Days past `max` are shown but disabled.
    expect(days[13]!.disabled).toBe(true);
  });
});
