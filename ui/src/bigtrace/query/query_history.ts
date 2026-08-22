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
import {Spinner} from '../../widgets/spinner';
import {EmptyState} from '../../widgets/empty_state';
import type {QueryExecution} from './query_store';
import {filterHistory, historyStore, type HistoryFilter} from './history_store';
import {renderHistoryItem, type OpenQueryFn} from './query_history_item';
import {renderSegmented} from '../widgets/segmented';

interface QueryHistoryComponentAttrs {
  readonly className?: string;
  openQuery: OpenQueryFn;
  readonly refreshSignal?: number;
}

// Kind filter above the list. Titles explain what each kind means, so the
// segment labels can stay one word.
const FILTERS: ReadonlyArray<{
  readonly key: HistoryFilter;
  readonly label: string;
  readonly title: string;
}> = [
  {key: 'all', label: 'All', title: 'Every query, newest first.'},
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
        renderSegmented(
          FILTERS.map((f) => ({
            key: f.key,
            label: `${f.label} (${filterHistory(historyStore.history, f.key).length})`,
            title: f.title,
          })),
          historyStore.filter,
          (key) => {
            historyStore.filter = key as HistoryFilter;
            m.redraw();
          },
          'pf-bt-history-filter',
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
    if (queries.length === 0) {
      return m(
        EmptyState,
        {
          title:
            historyStore.filter === 'all'
              ? 'No queries yet'
              : `No ${historyStore.filter} queries yet`,
          icon: 'search',
          fillHeight: true,
        },
        m(
          'div.pf-bt-history-empty-hint',
          historyStore.filter === 'persistent'
            ? 'Run a query with Persistent on to see it here.'
            : historyStore.filter === 'ephemeral'
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
