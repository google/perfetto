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
import {Icon} from '../../widgets/icon';
import {Spinner} from '../../widgets/spinner';
import {Tabs} from '../../widgets/tabs';
import type {BigTraceEditorTab, QueryTabsState} from './query_tabs_state';
import {renderStatusBox} from './status_box';
import {renderResultsGrid, shownRowCount} from './results_grid';

// Owns a 1s setInterval so the status bar's elapsed timer ticks while loading.
class RunningQuerySpinner implements m.ClassComponent {
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

  view(): m.Children {
    return m('.pf-empty-state.pf-empty-state--fill-height', [
      m('.pf-empty-state__title', 'Running query…'),
      m('.pf-empty-state__content', m(Spinner)),
    ]);
  }
}

export function renderResultsPanel(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  onRefresh?: () => void,
): m.Children {
  const status = renderStatusBox(tab, onRefresh);
  const renderSpinner = () => m(RunningQuerySpinner);

  const renderIdle = () =>
    m(EmptyState, {
      title: 'Run a query to see results',
      icon: 'search',
      fillHeight: true,
    });

  if (!tab.queryResult) {
    return m(
      '.pf-bt-results-panel',
      status,
      tab.isLoading ? renderSpinner() : renderIdle(),
    );
  }

  const processedRows = tab.execution?.processedRows ?? 0;
  const statusName = tab.execution?.status;
  const isInitialAsyncLoad =
    Boolean(tab.queryUuid) &&
    !tab.isLoading &&
    (statusName === undefined || statusName === 'UNKNOWN');
  const hasTable = Boolean(tab.execution?.tableName);

  const isLegacyUnsavedRun =
    tab.execution?.materialized === false &&
    !hasTable &&
    !tab.isLoading &&
    !tab.queryResult.error;

  const hasRowsToShow = processedRows > 0 && hasTable;

  const hasError = tab.queryResult.error !== undefined;

  let tableContent: m.Children;
  if (!tab.dataSource) {
    // Rejected before it ran (e.g. the per-user running-query limit).
    tableContent = renderIdle();
  } else if (isLegacyUnsavedRun) {
    tableContent = m(EmptyState, {
      title: 'Re-run the query to see results',
      icon: 'refresh',
      fillHeight: true,
    });
  } else if (hasRowsToShow) {
    tableContent = [
      renderPreviewBanner(tab),
      renderResultsGrid(tab, tabsState),
    ];
  } else if (tab.isLoading) {
    tableContent = renderSpinner();
  } else if (isInitialAsyncLoad) {
    tableContent = renderResultsGrid(tab, tabsState);
  } else if (processedRows > 0) {
    tableContent = m(EmptyState, {
      title: 'Results no longer available',
      icon: 'refresh',
      fillHeight: true,
    });
  } else if (statusName === 'CANCELLED') {
    tableContent = m(EmptyState, {
      title: 'Query was cancelled',
      icon: 'refresh',
      fillHeight: true,
    });
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

function renderPreviewBanner(tab: BigTraceEditorTab): m.Children {
  const status = tab.execution?.status;
  if (status === 'SUCCESS') return null;

  // The grid's count, so the banner never disagrees with the grid.
  const rows = shownRowCount(tab).toLocaleString();
  if (status === 'CANCELLED') {
    return m(
      '.pf-bt-preview-banner.pf-bt-preview-banner--warn',
      m(Icon, {icon: 'warning'}),
      m(
        'span',
        `Run cancelled. Showing the ${rows} preview rows; re-run the query to get the full result.`,
      ),
    );
  }
  if (status === 'FAILED') {
    const rawErr = tab.queryResult?.error ?? '';
    const firstLine = rawErr.split('\n')[0]?.trim() ?? '';
    return m(
      '.pf-bt-preview-banner.pf-bt-preview-banner--err',
      m(Icon, {icon: 'error'}),
      m(
        'span',
        `Full run failed${firstLine ? `: ${firstLine}` : ''}. Showing the ${rows} preview rows.`,
      ),
      m(
        'button.pf-bt-preview-banner__link',
        {
          type: 'button',
          onclick: () => {
            tab.resultsTabKey = 'error';
          },
        },
        'Open Error tab',
      ),
    );
  }
  return m(
    '.pf-bt-preview-banner',
    m(Icon, {icon: 'info'}),
    m(
      'span',
      `Showing the ${rows} preview rows while the full result is being prepared. The grid refreshes automatically when it is ready.`,
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
