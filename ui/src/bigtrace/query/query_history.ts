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
import {Icons} from '../../base/semantic_icons';
import {Accordion, AccordionSection} from '../../widgets/accordion';
import {Button} from '../../widgets/button';
import {Spinner} from '../../widgets/spinner';
import {EmptyState} from '../../widgets/empty_state';
import type {QueryExecution} from './query_store';
import {groupHistoryByDay, historyStore} from './history_store';
import {renderHistoryItem, type OpenQueryFn} from './query_history_item';

interface QueryHistoryComponentAttrs {
  readonly className?: string;
  openQuery: OpenQueryFn;
  readonly refreshSignal?: number;
}

export class QueryHistoryComponent implements m.ClassComponent<QueryHistoryComponentAttrs> {
  private allExpanded = true;
  private openGen = 0;

  oninit(vnode: m.CVnode<QueryHistoryComponentAttrs>) {
    historyStore.requestRefresh(vnode.attrs.refreshSignal ?? 0);
  }

  onbeforeupdate(vnode: m.CVnode<QueryHistoryComponentAttrs>) {
    historyStore.requestRefresh(vnode.attrs.refreshSignal ?? 0);
    return true;
  }

  view({attrs}: m.CVnode<QueryHistoryComponentAttrs>) {
    const {openQuery, ...rest} = attrs;
    const shown = historyStore.history;

    // The frame is unconditional: loading and error states render inside the
    // list, so the panel keeps its bounded layout and the refresh button stays
    // reachable — that button is how you retry a failed load.
    return m(
      '.pf-query-history',
      rest,
      m(
        '.pf-bt-history-toolbar',
        m(
          '.pf-bt-history-actions',
          m(Button, {
            icon: this.allExpanded ? Icons.UnfoldLess : Icons.UnfoldMore,
            title: this.allExpanded ? 'Collapse all' : 'Expand all',
            onclick: () => {
              this.allExpanded = !this.allExpanded;
              this.openGen++;
            },
          }),
          m(Button, {
            icon: 'refresh',
            title: 'Refresh history',
            onclick: () => historyStore.refreshNow(),
          }),
        ),
      ),
      m('.pf-bt-history-list', this.renderBody(shown, openQuery)),
    );
  }

  private renderBody(
    shown: QueryExecution[],
    openQuery: OpenQueryFn,
  ): m.Children {
    if (historyStore.isLoading && historyStore.history.length === 0) {
      return m(
        EmptyState,
        {title: 'Loading history…', icon: 'hourglass_empty', fillHeight: true},
        m(Spinner),
      );
    }
    if (historyStore.error !== null) {
      return m(
        EmptyState,
        {title: "Couldn't load history", icon: 'error', fillHeight: true},
        m('div.pf-bt-history-empty-hint', historyStore.error),
      );
    }
    return this.renderHistoryList(shown, openQuery);
  }

  private renderHistoryList(
    queries: QueryExecution[],
    openQuery?: OpenQueryFn,
  ): m.Children {
    if (queries.length === 0) {
      return m(
        EmptyState,
        {
          title: 'No queries yet',
          icon: 'search',
          fillHeight: true,
        },
        m('div.pf-bt-history-empty-hint', 'Queries you run show up here.'),
      );
    }

    return m(
      Accordion,
      {multi: true},
      groupHistoryByDay(queries).map((group) =>
        m(
          AccordionSection,
          {
            key: `${group.dayKey}-${this.openGen}`,
            defaultOpen: this.allExpanded,
            summary: `${group.label} (${group.entries.length})`,
          },
          group.entries.map((entry, index) =>
            renderHistoryItem(entry, index, openQuery),
          ),
        ),
      ),
    );
  }
}
