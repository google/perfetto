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
import {EmptyState} from '../../widgets/empty_state';
import {Spinner} from '../../widgets/spinner';
import {Tabs} from '../../widgets/tabs';
import {TERMINAL_STATUSES} from '../query/query_store';
import type {BigTraceEditorTab, QueryTabsState} from './query_tabs_state';
import {renderStatusBox, formatDurationS} from './status_box';
import {renderResultsGrid} from './results_grid';

// Owns its own setInterval since sync queries don't drive periodic redraws.
class RunningQuerySpinner implements m.ClassComponent<{startMs: number}> {
  private timer: number | null = null;

  oncreate(): void {
    this.timer = window.setInterval(() => m.redraw(), 1000);
  }

  onremove(): void {
    if (this.timer !== null) {
      window.clearInterval(this.timer);
      this.timer = null;
    }
  }

  view({attrs: {startMs}}: m.Vnode<{startMs: number}>): m.Children {
    const elapsedMs = Math.max(0, Date.now() - startMs);
    const durationStr = formatDurationS(elapsedMs);
    return m(
      EmptyState,
      {
        title: `Running query… ${durationStr}`,
        icon: 'hourglass_empty',
        fillHeight: true,
      },
      m(Spinner),
    );
  }
}

export function renderResultsPanel(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
): m.Children {
  const status = renderStatusBox(tab);

  if (!tab.dataSource || !tab.queryResult) {
    return m(
      '.pf-bt-results-panel',
      status,
      tab.isLoading
        ? m(RunningQuerySpinner, {startMs: tab.clientStartTime ?? Date.now()})
        : m(EmptyState, {
            title: 'Run a query to see results',
            icon: 'search',
            fillHeight: true,
          }),
    );
  }

  const processedRows = tab.execution?.processedRows ?? 0;

  const isSyncReopenNoRerun =
    !tab.materialize &&
    Boolean(tab.queryUuid) &&
    tab.queryResult.rows.length === 0 &&
    !tab.queryResult.error;

  const isTerminalStatus =
    tab.execution?.status !== undefined &&
    TERMINAL_STATUSES.has(tab.execution.status);
  const isAsyncTableCleared =
    tab.materialize &&
    Boolean(tab.queryUuid) &&
    !tab.execution?.tableName &&
    !tab.queryResult.error &&
    isTerminalStatus;

  const hasRowsToShow = tab.materialize
    ? processedRows > 0
    : tab.queryResult.rows.length > 0;

  const hasError = tab.queryResult.error !== undefined;

  let tableContent: m.Children;
  if (isSyncReopenNoRerun) {
    tableContent = m(EmptyState, {
      title: 'Re-run the query to see results',
      icon: 'refresh',
      fillHeight: true,
    });
  } else if (isAsyncTableCleared) {
    tableContent = m(EmptyState, {
      title:
        tab.execution?.status === 'CANCELLED'
          ? 'Query was cancelled'
          : 'Results no longer available',
      icon: 'refresh',
      fillHeight: true,
    });
  } else if (hasRowsToShow) {
    tableContent = renderResultsGrid(tab, tabsState);
  } else if (tab.isLoading) {
    tableContent = m('div');
  } else {
    tableContent = m(EmptyState, {
      title: 'Query returned no rows',
      icon: 'search',
      fillHeight: true,
    });
  }

  if (tab.resultsTabKey === 'error' && !hasError) {
    tab.resultsTabKey = undefined;
  }
  const defaultTab = hasError && !hasRowsToShow ? 'error' : 'table';
  const activeTab = tab.resultsTabKey ?? defaultTab;

  return m(
    '.pf-bt-results-panel',
    status,
    m(
      '.pf-bt-results-container',
      renderResultsTabs(tab, tableContent, activeTab),
    ),
  );
}

// ---------------------------------------------------------------------------
// Error / Table / Chart tabs.
// ---------------------------------------------------------------------------

function renderErrorTab(tab: BigTraceEditorTab): m.Children {
  const errorStr = tab.queryResult?.error ?? '';
  const fullText = errorStr
    .replaceAll('\\n', '\n')
    .replaceAll('\\t', '  ')
    .replaceAll('\\u003e', '>');
  return m('pre.pf-bt-error-content', fullText);
}

function renderResultsTabs(
  tab: BigTraceEditorTab,
  tableContent: m.Children,
  activeTab: string,
): m.Children {
  const hasError = tab.queryResult?.error !== undefined;
  const tabs = [
    ...(hasError
      ? [
          {
            key: 'error',
            title: m('span.pf-bt-error-tab-title', 'Error'),
            content: renderErrorTab(tab),
          },
        ]
      : []),
    {key: 'table', title: 'Table', content: tableContent},
    {
      key: 'chart',
      title: 'Chart',
      content: m(EmptyState, {
        title: 'Charts are coming soon',
        icon: 'bar_chart',
      }),
    },
  ];

  return m('.pf-bt-query-page__results', [
    m(Tabs, {
      tabs,
      activeTabKey: activeTab,
      onTabChange: (key) => {
        tab.resultsTabKey = key;
      },
    }),
  ]);
}
