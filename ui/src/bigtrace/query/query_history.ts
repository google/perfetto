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
import {Tabs, TabsTab} from '../../widgets/tabs';
import {Button} from '../../widgets/button';
import {Spinner} from '../../widgets/spinner';
import {EmptyState} from '../../widgets/empty_state';
import {QueryExecution} from './query_store';
import {historyStore} from './history_store';
import {renderHistoryItem, OpenQueryFn} from './query_history_item';

export {OpenQueryFn} from './query_history_item';

interface QueryHistoryComponentAttrs {
  readonly className?: string;
  openQuery: OpenQueryFn;
  readonly refreshSignal?: number;
}

// Re-export for existing consumers.
export {setHistoryActiveTab} from './history_store';

export class QueryHistoryComponent
  implements m.ClassComponent<QueryHistoryComponentAttrs>
{
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

    const standardQueries = historyStore.history.filter((h) => !h.materialized);
    const materializedQueries = historyStore.history.filter(
      (h) => h.materialized,
    );

    // Span-wrap titles so hover tooltips explain "Ephemeral"/"Persistent".
    const tabs: TabsTab[] = [
      {
        key: 'standard',
        title: m(
          'span',
          {
            title:
              'Queries run with Persistent OFF — results were shown ' +
              'inline at run time and not saved. Reopen here to see the ' +
              'SQL again or rerun.',
          },
          `Ephemeral (${standardQueries.length})`,
        ),
        content: this.renderHistoryList(standardQueries, false, openQuery),
      },
      {
        key: 'materialized',
        title: m(
          'span',
          {
            title:
              'Queries run with Persistent ON — results saved to a ' +
              'temporary backend table you can reopen and browse here.',
          },
          `Persistent (${materializedQueries.length})`,
        ),
        content: this.renderHistoryList(materializedQueries, true, openQuery),
      },
    ];

    return m(
      '.pf-query-history',
      rest,
      m(Tabs, {
        tabs: tabs,
        activeTabKey: historyStore.activeTabKey,
        onTabChange: (key) => {
          historyStore.activeTabKey = key;
          m.redraw();
        },
        rightContent: m(Button, {
          icon: 'refresh',
          title: 'Refresh history',
          onclick: () => historyStore.refreshNow(),
        }),
      }),
    );
  }

  private renderHistoryList(
    queries: QueryExecution[],
    isMaterialized: boolean,
    openQuery?: OpenQueryFn,
  ): m.Children {
    if (queries.length === 0) {
      return m(
        EmptyState,
        {
          title: isMaterialized
            ? 'No persistent queries yet'
            : 'No ephemeral queries yet',
          icon: 'search',
          fillHeight: true,
        },
        m(
          'div.pf-bt-history-empty-hint',
          isMaterialized
            ? 'Run a query with Persistent on to see it here.'
            : 'Run a query with Persistent off to see it here.',
        ),
      );
    }

    return queries.map((entry, index) =>
      renderHistoryItem(entry, index, isMaterialized, openQuery),
    );
  }
}
