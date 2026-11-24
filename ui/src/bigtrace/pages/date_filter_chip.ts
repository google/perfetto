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
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Popup, PopupPosition} from '../../widgets/popup';
import {type DateRange, todayIso} from '../base/iso_date';
import {
  type DateFilterState,
  QUICK_PICKS,
  describeDateFilter,
  resolveDateFilter,
} from '../query/date_filter';
import type {SettingsBindings} from '../settings/tab_bound_setting';
import {DateRangeCalendar} from '../widgets/date_range_calendar';

interface DateFilterChipAttrs {
  readonly bindings: SettingsBindings;
}

// Quick relative picks and "any date" apply at once; the calendars edit a
// fixed range that is written on Apply.
export class DateFilterChip implements m.ClassComponent<DateFilterChipAttrs> {
  private open = false;
  private draft: DateRange = {};

  view({attrs}: m.Vnode<DateFilterChipAttrs>): m.Children {
    const {bindings} = attrs;
    const today = todayIso();
    const state = bindings.getDateFilter();
    const close = () => (this.open = false);
    const write = (next: DateFilterState | null) => {
      bindings.setDateFilter(next);
      close();
    };
    const {draft} = this;
    return m(
      Popup,
      {
        isOpen: this.open,
        onChange: (open: boolean) => (this.open = open),
        position: PopupPosition.BottomEnd,
        fitContent: true,
        className: 'pf-bt-filter-popup',
        trigger: m(Button, {
          label: describeDateFilter(state, today),
          icon: 'date_range',
          variant: ButtonVariant.Outlined,
          intent: Intent.Primary,
          className: 'pf-bt-corpus-filters__chip',
          onclick: () => {
            // The calendars start from what is applied.
            this.draft = resolveDateFilter(state, today);
            this.open = !this.open;
          },
        }),
      },
      m('.pf-bt-date-dialog', [
        m('h4', [
          'Relative date range',
          m(Button, {
            label: 'Any date',
            compact: true,
            onclick: () => write(null),
          }),
        ]),
        m(
          '.pf-bt-date-dialog__picks',
          QUICK_PICKS.map((q) =>
            m(Button, {
              label: q.label,
              variant: ButtonVariant.Outlined,
              className: 'pf-bt-corpus-filters__chip',
              onclick: () => write({kind: 'last', days: q.days}),
            }),
          ),
        ),
        m('h4', 'Fixed date range'),
        m(DateRangeCalendar, {
          from: draft.from,
          to: draft.to,
          max: today,
          onChange: (range: DateRange) => (this.draft = range),
        }),
        m('.pf-bt-filter-dialog__footer', [
          m(
            'span',
            draft.from === undefined && draft.to === undefined
              ? 'Pick a start and an end day'
              : describeDateFilter({kind: 'fixed', ...draft}, today),
          ),
          m('span.pf-bt-filter-dialog__spacer'),
          m(Button, {
            label: 'Up to today',
            compact: true,
            onclick: () => (this.draft = {from: draft.from, to: today}),
          }),
          m(Button, {label: 'Cancel', onclick: close}),
          m(Button, {
            label: 'Apply',
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            disabled: draft.from === undefined && draft.to === undefined,
            onclick: () => write({kind: 'fixed', ...draft}),
          }),
        ]),
      ]),
    );
  }
}
