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

import m from 'mithril';
import {classNames} from '../../base/classnames';
import {
  daysInMonth,
  formatLongDate,
  formatMonth,
  isoDate,
  mondayBasedWeekday,
  shiftMonth,
  todayIso,
  type YearMonth,
} from '../base/iso_date';
import {Button} from '../../widgets/button';
import './calendar.scss';

export interface CalendarAttrs {
  readonly month: YearMonth;
  readonly onMonthChange: (month: YearMonth) => void;
  // Days ('YYYY-MM-DD') drawn as selected.
  readonly picked?: ReadonlyArray<string>;
  // Last pickable day (inclusive); later days are shown but disabled, and
  // the month navigation stops there.
  readonly max?: string;
  readonly isDisabled?: (day: string) => boolean;
  readonly onPick: (day: string) => void;
  // Optional caption on the left of the month navigation.
  readonly label?: m.Children;
}

// One month of a Monday-first calendar: month navigation, weekday header and
// a grid of day buttons. Stateless; the month shown is owned by the parent.
export class Calendar implements m.ClassComponent<CalendarAttrs> {
  view({attrs}: m.Vnode<CalendarAttrs>): m.Children {
    const {month, picked = [], max, isDisabled} = attrs;
    const today = todayIso();
    const first = isoDate(month.year, month.month, 1);
    const nextMonth = shiftMonth(month, 1);
    const canGoNext =
      max === undefined || isoDate(nextMonth.year, nextMonth.month, 1) <= max;
    const days: m.Children[] = [];
    for (let i = 0; i < mondayBasedWeekday(first); i++) {
      days.push(m('.pf-bt-calendar__day.pf-bt-calendar__day--blank'));
    }
    for (let d = 1; d <= daysInMonth(month); d++) {
      const iso = isoDate(month.year, month.month, d);
      const isPicked = picked.includes(iso);
      days.push(
        m(
          'button.pf-bt-calendar__day',
          {
            // Never a submit button, even inside a form.
            'type': 'button',
            'disabled':
              (max !== undefined && iso > max) || isDisabled?.(iso) === true,
            'className': classNames(
              isPicked && 'pf-bt-calendar__day--picked',
              iso === today && 'pf-bt-calendar__day--today',
            ),
            // The visible text is just the day number; give assistive tech
            // the full date and the selection state.
            'aria-label': formatLongDate(iso),
            'aria-pressed': isPicked ? 'true' : 'false',
            'onclick': () => attrs.onPick(iso),
          },
          d,
        ),
      );
    }
    return m('.pf-bt-calendar', [
      m('.pf-bt-calendar__nav', [
        attrs.label !== undefined && m('span', attrs.label),
        m('span.pf-bt-calendar__month-name', formatMonth(month)),
        m(Button, {
          icon: 'chevron_left',
          compact: true,
          title: 'Previous month',
          onclick: () => attrs.onMonthChange(shiftMonth(month, -1)),
        }),
        m(Button, {
          icon: 'chevron_right',
          compact: true,
          title: 'Next month',
          disabled: !canGoNext,
          onclick: () => attrs.onMonthChange(nextMonth),
        }),
      ]),
      m(
        '.pf-bt-calendar__weekdays',
        ['M', 'T', 'W', 'T', 'F', 'S', 'S'].map((w) => m('span', w)),
      ),
      m('.pf-bt-calendar__days', days),
    ]);
  }
}
