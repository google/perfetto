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
import {Button} from '../../widgets/button';
import {Checkbox} from '../../widgets/checkbox';
import {Spinner} from '../../widgets/spinner';
import {EmptyState} from '../../widgets/empty_state';
import type {QueryExecution} from './query_store';
import {filterHistory, historyStore} from './history_store';
import {renderHistoryItem, type OpenQueryFn} from './query_history_item';

interface QueryHistoryComponentAttrs {
  readonly className?: string;
  openQuery: OpenQueryFn;
  readonly refreshSignal?: number;
}

// Kind filter above the list: two independent checkboxes, so "everything" is
// just both ticked and there's no redundant "All" to spend the row on.
const KINDS: ReadonlyArray<{
  readonly key: 'ephemeral' | 'persistent';
  readonly label: string;
  readonly title: string;
}> = [
  {
    key: 'ephemeral',
    label: 'Ephemeral',
    title:
      'Queries run with Persistent off — results were shown inline at run ' +
      'time and not saved. Reopen to see the SQL again or rerun.',
  },
  {
    key: 'persistent',
    label: 'Persistent',
    title:
      'Queries run with Persistent on — results saved to a temporary ' +
      'backend table you can reopen and browse.',
  },
];

export class QueryHistoryComponent implements m.ClassComponent<QueryHistoryComponentAttrs> {
  oninit(vnode: m.CVnode<QueryHistoryComponentAttrs>) {
    historyStore.requestRefresh(vnode.attrs.refreshSignal ?? 0);
  }

  onbeforeupdate(vnode: m.CVnode<QueryHistoryComponentAttrs>) {
    historyStore.requestRefresh(vnode.attrs.refreshSignal ?? 0);
    return true;
  }

  view({attrs}: m.CVnode<QueryHistoryComponentAttrs>) {
    const {openQuery, ...rest} = attrs;

    if (historyStore.isLoading && historyStore.history.length === 0) {
      return m(
        EmptyState,
        {
          title: 'Loading history...',
          icon: 'hourglass_empty',
          fillHeight: true,
        },
        m(Spinner),
      );
    }

    if (historyStore.error) {
      return m(EmptyState, {
        title: `Failed to load history: ${historyStore.error}`,
        icon: 'error',
        fillHeight: true,
      });
    }

    const shown = filterHistory(historyStore.history, historyStore.filter);

    return m(
      '.pf-query-history',
      rest,
      m(
        '.pf-bt-history-toolbar',
        m(
          '.pf-bt-history-filter',
          KINDS.map((kind) => {
            const checked = historyStore.filter[kind.key];
            const count = historyStore.history.filter(
              (h) => (h.materialized === true) === (kind.key === 'persistent'),
            ).length;
            return m(Checkbox, {
              label: `${kind.label} (${count})`,
              title: kind.title,
              checked,
              onchange: () => {
                historyStore.filter = {
                  ...historyStore.filter,
                  [kind.key]: !checked,
                };
                m.redraw();
              },
            });
          }),
        ),
        m(Button, {
          icon: 'refresh',
          title: 'Refresh history',
          onclick: () => historyStore.refreshNow(),
        }),
      ),
      m('.pf-bt-history-list', this.renderHistoryList(shown, openQuery)),
    );
  }

  private renderHistoryList(
    queries: QueryExecution[],
    openQuery?: OpenQueryFn,
  ): m.Children {
    const {ephemeral, persistent} = historyStore.filter;
    if (!ephemeral && !persistent) {
      return m(
        EmptyState,
        {title: 'Nothing selected', icon: 'filter_alt_off', fillHeight: true},
        m(
          'div.pf-bt-history-empty-hint',
          'Tick Ephemeral or Persistent to see queries.',
        ),
      );
    }
    if (queries.length === 0) {
      return m(
        EmptyState,
        {
          title: 'No queries yet',
          icon: 'search',
          fillHeight: true,
        },
        m(
          'div.pf-bt-history-empty-hint',
          !ephemeral
            ? 'Run a query with Persistent on to see it here.'
            : !persistent
              ? 'Run a query with Persistent off to see it here.'
              : 'Queries you run show up here.',
        ),
      );
    }

    return queries.map((entry, index) =>
      renderHistoryItem(entry, index, openQuery),
    );
  }
}
