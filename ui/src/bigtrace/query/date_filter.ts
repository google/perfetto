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

// The upload-date range a tab queries over: its state, how it resolves into
// the `>=` / `<=` pair of trace filters a request carries, and how it is
// read back out of a run's filters. The chip that edits it lives in
// pages/date_filter_chip.ts.

import type {Filter} from '../../components/widgets/datagrid/model';
import {addDays, type DateRange, formatShortDate} from '../base/iso_date';

export const UPLOAD_DATE_COLUMN = 'upload_date';

// A relative range is re-resolved against today on every request, so a tab
// opened next week still means "the last 7 days". `null` is "any date";
// `undefined` means the default has not been applied yet.
export type DateFilterState =
  | {readonly kind: 'last'; readonly days: number}
  | ({readonly kind: 'fixed'} & DateRange);

export const DEFAULT_DATE_FILTER: DateFilterState = {kind: 'last', days: 7};

export const QUICK_PICKS: ReadonlyArray<{days: number; label: string}> = [
  {days: 7, label: 'Last 7 days'},
  {days: 14, label: 'Last 2 weeks'},
  {days: 28, label: 'Last 4 weeks'},
  {days: 56, label: 'Last 8 weeks'},
  {days: 84, label: 'Last 12 weeks'},
];

export function resolveDateFilter(
  state: DateFilterState | null | undefined,
  today: string,
): DateRange {
  if (state == null) return {};
  if (state.kind === 'last') {
    return {from: addDays(today, 1 - state.days), to: today};
  }
  return {from: state.from, to: state.to};
}

// The grid filters a range stands for: the >= / <= pair on the date column.
export function dateFilterToFilters(
  state: DateFilterState | null | undefined,
  today: string,
): Filter[] {
  const {from, to} = resolveDateFilter(state, today);
  const out: Filter[] = [];
  if (from !== undefined) {
    out.push({field: UPLOAD_DATE_COLUMN, op: '>=', value: from});
  }
  if (to !== undefined) {
    out.push({field: UPLOAD_DATE_COLUMN, op: '<=', value: to});
  }
  return out;
}

function sameFilter(a: Filter, b: Filter): boolean {
  return (
    a.field === b.field &&
    a.op === b.op &&
    'value' in a &&
    'value' in b &&
    a.value === b.value
  );
}

// Takes the date pair back out of a grid filter list. `intact` is false when
// the user removed one of them in the grid, i.e. left the range. Checked by
// membership, not by count: a grid filter that happens to duplicate one half
// of the pair is swept out with it without being read as a removal.
export function splitDateFilters(
  filters: ReadonlyArray<Filter>,
  dateFilters: ReadonlyArray<Filter>,
): {rest: Filter[]; intact: boolean} {
  const rest = filters.filter(
    (f) => !dateFilters.some((d) => sameFilter(d, f)),
  );
  const intact = dateFilters.every((d) =>
    filters.some((f) => sameFilter(d, f)),
  );
  return {rest, intact};
}

// Pulls an absolute upload-date pair out of a run's trace filters (a history
// re-run), so the chip shows the dates that run used.
export function extractDateFilter(filters: ReadonlyArray<Filter>): {
  rest: Filter[];
  dateFilter: DateFilterState | undefined;
} {
  let from: string | undefined;
  let to: string | undefined;
  const rest = filters.filter((f) => {
    if (f.field !== UPLOAD_DATE_COLUMN || !('value' in f)) return true;
    if (typeof f.value !== 'string') return true;
    if (f.op === '>=' && from === undefined) {
      from = f.value;
      return false;
    }
    if (f.op === '<=' && to === undefined) {
      to = f.value;
      return false;
    }
    return true;
  });
  const dateFilter =
    from === undefined && to === undefined
      ? undefined
      : {kind: 'fixed' as const, from, to};
  return {rest, dateFilter};
}

export function describeDateFilter(
  state: DateFilterState | null | undefined,
  today: string,
): string {
  const {from, to} = resolveDateFilter(state, today);
  if (state != null && from !== undefined && to !== undefined) {
    const span = `${formatShortDate(from)} – ${formatShortDate(to)}`;
    if (state.kind === 'fixed') return span;
    const quick = QUICK_PICKS.find((q) => q.days === state.days);
    return `${quick?.label ?? `Last ${state.days} days`} (${span})`;
  }
  if (from !== undefined) return `From ${formatShortDate(from)}`;
  if (to !== undefined) return `Until ${formatShortDate(to)}`;
  return 'Upload date: Any';
}
