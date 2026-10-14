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
import type {Filter} from '../../components/widgets/datagrid/model';
import {Button} from '../../widgets/button';
import {Checkbox} from '../../widgets/checkbox';
import {Intent} from '../../widgets/common';
import {Spinner} from '../../widgets/spinner';
import {QueryCancelledError} from '../query/bigtrace_query_client';
import {
  type ColumnValuesLoader,
  VALUE_LIST_LIMIT,
  describeLoadError,
} from '../query/column_values_loader';
import {encodeFilters} from '../query/filter_encoding';

// The label with the searched substring wrapped in <mark>.
export function highlight(label: string, search: string): m.Children {
  if (search === '') return label;
  const i = label.toLowerCase().indexOf(search.toLowerCase());
  if (i < 0) return label;
  return [
    label.slice(0, i),
    m('mark', label.slice(i, i + search.length)),
    label.slice(i + search.length),
  ];
}

export interface ValueListAttrs {
  readonly column: string;
  readonly load: ColumnValuesLoader;
  // What the list is narrowed by (see facetFilters in value_set_filters.ts).
  readonly facet: ReadonlyArray<Filter>;
  readonly search?: string;
  readonly selected: ReadonlySet<string>;
  readonly onToggle: (value: string, checked: boolean) => void;
  // Keep picked values at the top, sorted, even when the search or facet no
  // longer returns them: for long, searchable lists. Off (the default, as
  // in the MultiSelect widget), rows keep the backend's order and picked
  // values it did not return are appended at the end, so they can always
  // be unticked either way.
  readonly pinSelected?: boolean;
}

// Typing pauses this long before a search is sent.
export const SEARCH_DEBOUNCE_MS = 250;

function matchesSearch(value: string, search: string): boolean {
  return search === '' || value.toLowerCase().includes(search.toLowerCase());
}

// The rows to show for one answer: the fetched values that match the live
// search, with every picked value kept in — on top (sorted) when pinning,
// else in place, with the ones the backend did not return at the end.
export function orderValues(
  fetched: ReadonlyArray<string>,
  selected: ReadonlySet<string>,
  search: string,
  pinSelected: boolean,
): string[] {
  if (pinSelected) {
    return [
      ...[...selected].sort(),
      ...fetched.filter((v) => !selected.has(v) && matchesSearch(v, search)),
    ];
  }
  const shown = fetched.filter(
    (v) => selected.has(v) || matchesSearch(v, search),
  );
  const seen = new Set(shown);
  return [...shown, ...[...selected].filter((v) => !seen.has(v)).sort()];
}

// A checkbox list of one column's values. Picked values are never dropped
// from the list (pinned on top or appended at the end, see `pinSelected`),
// so they can always be unticked. Requests are independent, so a newer one
// aborts the one in flight; the last list for the same column+facet stays
// visible meanwhile, narrowed locally by the live search text (same
// substring rule as the backend) so stale non-matches don't linger. A
// failure is remembered only for the exact column+facet+search it happened
// for, and only until the user asks for something else or presses Retry.
export class ValueList implements m.ClassComponent<ValueListAttrs> {
  private inflight?: {key: string; controller: AbortController};
  private fetched?: {key: string; scope: string; values: ReadonlyArray<string>};
  private failed?: {key: string; error: unknown};
  // The search the current fetch key uses; trails the live text by the
  // debounce.
  private sentSearch = '';
  private debounce?: {search: string; timer: ReturnType<typeof setTimeout>};

  view({attrs}: m.Vnode<ValueListAttrs>): m.Children {
    const search = attrs.search ?? '';
    this.settleSearch(search);
    const scope = JSON.stringify({
      column: attrs.column,
      facet: encodeFilters(attrs.facet),
    });
    const key = `${scope}\n${this.sentSearch}`;
    if (this.failed !== undefined && this.failed.key !== key) {
      this.failed = undefined;
    }
    if (
      this.inflight?.key !== key &&
      this.fetched?.key !== key &&
      this.failed === undefined
    ) {
      this.fetch(attrs, key, scope);
    }
    const stale = this.fetched?.scope === scope ? this.fetched : undefined;
    const fetched = stale?.values ?? [];
    const shown = orderValues(
      fetched,
      attrs.selected,
      search,
      attrs.pinSelected === true,
    );
    const busy = this.inflight !== undefined;
    return m('.pf-bt-value-list', [
      busy &&
        m(
          '.pf-bt-value-list__note.pf-bt-value-list__note--busy',
          {role: 'status'},
          [m(Spinner), stale === undefined ? ' Loading…' : ' Updating…'],
        ),
      // Same markup as the stock MultiSelect: one labelled Checkbox per row.
      shown.map((value) =>
        m(Checkbox, {
          key: value,
          className: 'pf-bt-value-list__item',
          label: highlight(value, search),
          checked: attrs.selected.has(value),
          onchange: () => attrs.onToggle(value, !attrs.selected.has(value)),
        }),
      ),
      this.failed !== undefined
        ? this.renderFailure(this.failed.error)
        : !busy &&
          shown.length === 0 &&
          m('.pf-bt-value-list__note', 'No values match.'),
      fetched.length >= VALUE_LIST_LIMIT &&
        m(
          '.pf-bt-value-list__more',
          `Showing the first ${VALUE_LIST_LIMIT} — type to narrow down.`,
        ),
    ]);
  }

  private renderFailure(error: unknown): m.Children {
    const {summary, detail} = describeLoadError(error);
    return m(
      '.pf-bt-value-list__note.pf-bt-value-list__note--error',
      {role: 'alert', title: detail},
      [
        m('div', [
          'Couldn’t load values. ',
          m(Button, {
            label: 'Retry',
            compact: true,
            intent: Intent.Primary,
            // Forgetting the failure is enough: the next view() refetches.
            onclick: () => (this.failed = undefined),
          }),
        ]),
        m('.pf-bt-value-list__reason', summary),
      ],
    );
  }

  private settleSearch(search: string): void {
    if (search === this.sentSearch) {
      if (this.debounce) clearTimeout(this.debounce.timer);
      this.debounce = undefined;
      return;
    }
    if (this.debounce?.search === search) return;
    if (this.debounce) clearTimeout(this.debounce.timer);
    this.debounce = {
      search,
      timer: setTimeout(() => {
        this.sentSearch = search;
        this.debounce = undefined;
        m.redraw();
      }, SEARCH_DEBOUNCE_MS),
    };
  }

  private fetch(attrs: ValueListAttrs, key: string, scope: string): void {
    this.inflight?.controller.abort();
    const controller = new AbortController();
    this.inflight = {key, controller};
    attrs
      .load(attrs.column, attrs.facet, this.sentSearch, controller.signal)
      .then(
        (values) => {
          if (this.inflight?.controller !== controller) return;
          this.inflight = undefined;
          this.fetched = {key, scope, values};
          m.redraw();
        },
        (e) => {
          if (this.inflight?.controller !== controller) return;
          this.inflight = undefined;
          // Our own aborts (a newer fetch, or onremove) have already replaced
          // or cleared `inflight`, so a cancellation here is unexpected; it
          // is not a failure to show either way.
          if (!(e instanceof QueryCancelledError)) {
            this.failed = {key, error: e};
          }
          m.redraw();
        },
      );
  }

  onremove(): void {
    // Drop the references too, so the cancellation's rejection fails the
    // identity check above and nothing redraws for an unmounted list.
    this.inflight?.controller.abort();
    this.inflight = undefined;
    if (this.debounce) clearTimeout(this.debounce.timer);
    this.debounce = undefined;
  }
}
