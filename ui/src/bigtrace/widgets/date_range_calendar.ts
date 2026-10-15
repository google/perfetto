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
import {
  type DateRange,
  shiftMonth,
  todayIso,
  type YearMonth,
  yearMonthOf,
} from '../base/iso_date';
import {Calendar} from './calendar';
import './date_range_calendar.scss';

export type RangeSide = 'start' | 'end';

// Applies one calendar click. The left calendar sets the start (dropping an
// end that would now precede it); the right one sets the end and ignores days
// before the start.
export function pickDate(
  range: DateRange,
  side: RangeSide,
  day: string,
): DateRange {
  if (side === 'start') {
    const to = range.to !== undefined && range.to < day ? undefined : range.to;
    return {from: day, to};
  }
  if (range.from !== undefined && day < range.from) return range;
  return {from: range.from, to: day};
}

export interface DateRangeCalendarAttrs extends DateRange {
  // Last pickable day (inclusive).
  readonly max?: string;
  readonly onChange: (range: DateRange) => void;
}

// Two Calendars side by side: the left one picks the start of the range, the
// right one its end. Each has its own month navigation.
export class DateRangeCalendar implements m.ClassComponent<DateRangeCalendarAttrs> {
  private startMonth!: YearMonth;
  private endMonth!: YearMonth;
  private lastRange: DateRange = {};

  oninit({attrs}: m.Vnode<DateRangeCalendarAttrs>) {
    const anchor = attrs.max ?? todayIso();
    this.endMonth = yearMonthOf(attrs.to ?? anchor);
    this.startMonth = attrs.from
      ? yearMonthOf(attrs.from)
      : shiftMonth(this.endMonth, -1);
    this.lastRange = {from: attrs.from, to: attrs.to};
  }

  // A range set from outside (a quick pick) moves the calendars onto it.
  onbeforeupdate({attrs}: m.Vnode<DateRangeCalendarAttrs>) {
    if (attrs.from !== this.lastRange.from && attrs.from !== undefined) {
      this.startMonth = yearMonthOf(attrs.from);
    }
    if (attrs.to !== this.lastRange.to && attrs.to !== undefined) {
      this.endMonth = yearMonthOf(attrs.to);
    }
    this.lastRange = {from: attrs.from, to: attrs.to};
  }

  view({attrs}: m.Vnode<DateRangeCalendarAttrs>): m.Children {
    const {from, to, max} = attrs;
    // Each calendar marks only its own end of the range, so a range inside
    // one month doesn't show the same two days twice.
    return m('.pf-bt-date-range-calendar', [
      m(Calendar, {
        label: 'Start',
        month: this.startMonth,
        onMonthChange: (ym) => (this.startMonth = ym),
        picked: from === undefined ? [] : [from],
        max,
        onPick: (day) => attrs.onChange(pickDate({from, to}, 'start', day)),
      }),
      m(Calendar, {
        label: 'End',
        month: this.endMonth,
        onMonthChange: (ym) => (this.endMonth = ym),
        picked: to === undefined ? [] : [to],
        max,
        isDisabled: (day) => from !== undefined && day < from,
        onPick: (day) => attrs.onChange(pickDate({from, to}, 'end', day)),
      }),
    ]);
  }
}
