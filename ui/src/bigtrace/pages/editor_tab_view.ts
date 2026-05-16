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
import {Box} from '../../widgets/box';
import {Button, ButtonVariant} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {Editor} from '../../widgets/editor';
import {EmptyState} from '../../widgets/empty_state';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {linkify} from '../../widgets/anchor';
import {Spinner} from '../../widgets/spinner';
import {SplitPanel} from '../../widgets/split_panel';
import {PopupPosition} from '../../widgets/popup';
import {Stack, StackAuto} from '../../widgets/stack';
import {Tooltip} from '../../widgets/tooltip';
import {Switch} from '../../widgets/switch';
import {Tabs} from '../../widgets/tabs';
import {TextInput} from '../../widgets/text_input';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {DataSource} from '../../components/widgets/datagrid/data_source';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {
  ColumnSchema,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import {Duration} from '../../base/time';
import {endpointStorage} from '../settings/endpoint_storage';
import {SettingFilter} from '../settings/settings_types';
import {BigtraceAsyncDataSource} from '../query/bigtrace_async_data_source';
import {setHistoryActiveTab} from '../query/query_history';
import {BigtraceQueryClient} from '../query/bigtrace_query_client';
import {QueryRunner} from '../query/query_runner';
import {
  formatCompact,
  queryStore,
  statusDisplayLabel,
  TERMINAL_STATUSES,
} from '../query/query_store';
import {
  BigTraceEditorTab,
  QueryResponse,
  QueryTabsState,
  deriveTitleFromQuery,
} from './query_tabs_state';

export interface EditorTabViewAttrs {
  readonly tab: BigTraceEditorTab;
  readonly tabsState: QueryTabsState;
  readonly runner: QueryRunner;
  readonly useBigtraceBackend: boolean;
}

// Purely presentational; state on `tab`, side-effects via `runner`/`tabsState`.
export class EditorTabView implements m.ClassComponent<EditorTabViewAttrs> {
  view({attrs}: m.Vnode<EditorTabViewAttrs>): m.Children {
    const {tab, tabsState, runner, useBigtraceBackend} = attrs;

    // Tabs reopened from history wire up their dataSource on first render.
    if (tab.queryUuid && !tab.dataSource) {
      attachAsyncDataSource(tab, runner);
    }

    if (tab.dataSource && tab.queryResult && tab.materialize && tab.execution) {
      tab.queryResult.totalRowCount = tab.execution.processedRows;
    }

    return m(SplitPanel, {
      direction: 'vertical',
      // Most BigTrace queries are short; bias the split toward results.
      initialSplit: {percent: 22},
      minSize: 100,
      firstPanel: renderEditorPanel(tab, tabsState, runner, useBigtraceBackend),
      secondPanel: renderResultsPanel(tab, tabsState, runner),
    });
  }
}

// ---------------------------------------------------------------------------
// Editor panel: toolbar (Run/Cancel + limit + Materialize) and the editor.
// ---------------------------------------------------------------------------

function renderEditorPanel(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  runner: QueryRunner,
  useBigtraceBackend: boolean,
): m.Children {
  return m('.pf-query-page__editor-panel', [
    m(Box, {className: 'pf-query-page__toolbar'}, [
      m(Stack, {orientation: 'horizontal'}, [
        tab.isLoading
          ? m(Button, {
              label: 'Cancel',
              icon: 'stop',
              intent: Intent.Warning,
              variant: ButtonVariant.Filled,
              onclick: () => runner.cancel(tab),
            })
          : m(Button, {
              label: 'Run Query',
              icon: 'play_arrow',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
              // Whitespace + SQL line comments → nothing to run.
              disabled: deriveTitleFromQuery(tab.editorText) === undefined,
              onclick: () => {
                setHistoryActiveTab(tab.materialize);
                tabsState.maybeAutoNameTab(tab.id, tab.editorText);
                runner.run(tab, tab.editorText);
              },
            }),
        m(
          Stack,
          {orientation: 'horizontal', className: 'pf-query-page__hotkeys'},
          'or press',
          m(HotkeyGlyphs, {hotkey: 'Mod+Enter'}),
        ),
        m(StackAuto),
        useBigtraceBackend && [
          m(Switch, {
            label: 'Persistent',
            title:
              'ON: results saved to History (Persistent tab) — reopen later. ' +
              'OFF: results shown inline and discarded when the tab closes.',
            checked: tab.materialize,
            // Mode captured at submit; disable mid-run so it isn't a false affordance.
            disabled: tab.isLoading,
            onchange: (e: Event) => {
              tab.materialize = (e.target as HTMLInputElement).checked;
              setHistoryActiveTab(tab.materialize);
              tabsState.markDirty();
            },
          }),
          m('span.pf-query-page__toolbar-divider', {'aria-hidden': 'true'}),
          m('span', 'Limit:'),
          m(TextInput, {
            type: 'number',
            value: String(tab.limit),
            placeholder: 'Limit',
            // Captured at submit; disable mid-run.
            disabled: tab.isLoading,
            onInput: (value: string) => {
              const newLimit = parseInt(value, 10);
              if (!isNaN(newLimit) && newLimit > 0) {
                tab.limit = newLimit;
              }
            },
          }),
        ],
      ]),
    ]),
    tab.editorText.includes('"') &&
      m(
        Callout,
        {icon: 'warning', intent: Intent.None},
        `" (double quote) character observed in query; if this is being used to ` +
          `define a string, please use ' (single quote) instead. Using double quotes ` +
          `can cause subtle problems which are very hard to debug.`,
      ),
    m(Editor, {
      text: tab.editorText,
      language: 'perfetto-sql',
      autofocus: true,
      // Claim Ctrl/Cmd+S so the browser's "Save Page As…" doesn't fire.
      onSave: () => {},
      onUpdate: (text: string) => {
        tab.editorText = text;
        tabsState.markDirty();
      },
      onExecute: (query: string) => {
        setHistoryActiveTab(tab.materialize);
        tabsState.maybeAutoNameTab(tab.id, query);
        runner.run(tab, query);
      },
    }),
  ]);
}

// "<1s" for sub-500ms runs so the user sees the query actually ran.
function formatDurationS(ms: number): string {
  if (ms < 500) return '<1s';
  return Duration.format(Duration.fromMillis(Math.round(ms / 1000) * 1000));
}

// Owns its own setInterval since sync queries don't drive periodic redraws.
class RunningQuerySpinner implements m.ClassComponent<{startMs: number}> {
  private timer: number | null = null;

  oncreate(): void {
    this.timer = window.setInterval(() => m.redraw(), 100);
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

// ---------------------------------------------------------------------------
// Results panel: status box (async only) + result tabs (Error/Table/Chart).
// ---------------------------------------------------------------------------

function renderResultsPanel(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  runner: QueryRunner,
): m.Children {
  const status = renderStatusBox(tab);

  if (!tab.dataSource || !tab.queryResult) {
    return m(
      '.pf-query-page__results-panel',
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

  // Sync re-open from history (rows not persisted): show a re-run hint
  // instead of the misleading "no rows" empty state.
  const isSyncReopenNoRerun =
    !tab.materialize &&
    Boolean(tab.queryUuid) &&
    tab.queryResult.rows.length === 0 &&
    !tab.queryResult.error;

  // Materialized table gone (TTL / cancellation / post-failure) but
  // metadata still claims rows; without this we'd spin on "Loading
  // schema…". Only after terminal; mid-run the table is still building.
  const isTerminalStatus =
    tab.execution?.status !== undefined &&
    TERMINAL_STATUSES.has(tab.execution.status);
  const isAsyncTableCleared =
    tab.materialize &&
    Boolean(tab.queryUuid) &&
    !tab.execution?.tableName &&
    !tab.queryResult.error &&
    isTerminalStatus;

  // Async: live processedRows. Sync: inline rows (processedRows is 0 server-side).
  const hasRowsToShow = tab.materialize
    ? processedRows > 0
    : tab.queryResult.rows.length > 0;

  const hasError = tab.queryResult.error !== undefined;

  // Table content: the primary result display.
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
    tableContent = renderResultsGrid(tab, tabsState, runner);
  } else if (tab.isLoading) {
    tableContent = m('div');
  } else {
    tableContent = m(EmptyState, {
      title: 'Query returned no rows',
      icon: 'search',
      fillHeight: true,
    });
  }

  // Auto-select Error tab when it's the only signal; user override sticks.
  // Reset stale key if the Error tab no longer exists.
  if (tab.resultsTabKey === 'error' && !hasError) {
    tab.resultsTabKey = undefined;
  }
  const defaultTab = hasError && !hasRowsToShow ? 'error' : 'table';
  const activeTab = tab.resultsTabKey ?? defaultTab;

  return m(
    '.pf-query-page__results-panel',
    status,
    m(
      '.pf-query-page__results-container',
      renderResultsTabs(tab, tableContent, activeTab),
    ),
  );
}

function renderStatusBox(tab: BigTraceEditorTab): m.Children {
  if (!tab.materialize || !tab.queryUuid) return false;

  const isTerminal =
    tab.execution?.status !== undefined &&
    TERMINAL_STATUSES.has(tab.execution.status);
  const processedRows = tab.execution?.processedRows ?? 0;
  const hasNewData = !isTerminal && processedRows > tab.lastProcessedRows;

  let durationMs = 0;
  if (
    isTerminal &&
    tab.execution?.endTime !== undefined &&
    tab.execution?.startTime !== undefined
  ) {
    durationMs = tab.execution.endTime - tab.execution.startTime;
  } else if (!isTerminal) {
    const start =
      tab.execution?.startTime !== undefined
        ? tab.execution.startTime
        : tab.clientStartTime;
    if (start !== undefined) {
      durationMs = Date.now() - start;
    }
  }

  const status = tab.execution?.status ?? 'UNKNOWN';
  const processedTraces = tab.execution?.processedTraces ?? 0;
  const totalTraces = tab.execution?.totalTraces ?? 0;
  const durationStr = formatDurationS(durationMs);

  // Left group: refresh, status pill, duration.
  const leftGroup = m(
    '.pf-query-page__status-bar-group',
    m(
      'div.pf-query-page__status-bar-refresh',
      m(Button, {
        icon: 'refresh',
        title: hasNewData
          ? 'New data available. Click to refresh.'
          : 'Refresh data',
        onclick: () => refreshAsyncStatus(tab),
      }),
      hasNewData &&
        m('span.pf-query-page__status-bar-notif', {
          'aria-label': 'New data available',
        }),
    ),
    m(
      'span.pf-query-page__status-bar-pill',
      {className: `pf-status-${status.toLowerCase().replace(/_/g, '-')}`},
      statusDisplayLabel(status),
    ),
    m(
      'span.pf-query-page__status-bar-duration',
      m('span.pf-query-page__status-bar-duration-value', durationStr),
    ),
  );

  // Right group: progress bars (Traces and Rows). While running, the CSS
  // collapses labels/values to opacity 0 and hides the Traces bar so the
  // user just sees the Rows progress bar; hovering reveals the numbers.
  const rowsStatClasses = [
    'pf-query-page__status-bar-stat',
    'pf-query-page__status-bar-stat--rows',
    processedRows === 0 && 'pf-query-page__status-bar-stat--empty',
  ]
    .filter(Boolean)
    .join(' ');
  const rightGroupContent = m(
    '.pf-query-page__status-bar-group',
    m(
      'span.pf-query-page__status-bar-stat.pf-query-page__status-bar-stat--traces',
      m('span.pf-query-page__status-bar-stat-label', 'Traces:'),
      m(
        'span.pf-query-page__status-bar-stat-value',
        {
          // Tooltip preserves exact counts; the displayed values are compact.
          title:
            `${processedTraces.toLocaleString()} of ` +
            `${totalTraces.toLocaleString()}` +
            (!isTerminal
              ? ' — numerator lags the poll (≤3s); denominator is exact.'
              : ''),
        },
        formatCompact(processedTraces),
      ),
      renderInlineProgressBar(processedTraces, totalTraces, !isTerminal),
    ),
    m(
      'span',
      {className: rowsStatClasses},
      m('span.pf-query-page__status-bar-stat-label', 'Rows:'),
      m(
        'span.pf-query-page__status-bar-stat-value',
        {
          title: `${processedRows.toLocaleString()} of result limit ${tab.limit.toLocaleString()}`,
        },
        formatCompact(processedRows),
      ),
      renderInlineProgressBar(processedRows, tab.limit, !isTerminal),
    ),
  );

  // While running, wrap the whole right group in a Tooltip — hovering
  // anywhere on the right side reveals TRACES + ROWS counts that are
  // collapsed to just the progress bar by default.
  const rightGroup = !isTerminal
    ? m(
        Tooltip,
        {
          trigger: rightGroupContent,
          position: PopupPosition.Top,
        },
        m(
          '.pf-query-page__status-bar-progress-tooltip',
          m('div', `Traces: ${formatCompact(processedTraces)}`),
          m('div', `Rows: ${formatCompact(processedRows)}`),
        ),
      )
    : rightGroupContent;

  return m(
    Box,
    {
      className: isTerminal
        ? 'pf-query-page__status-bar'
        : 'pf-query-page__status-bar pf-query-page__status-bar--running',
    },
    leftGroup,
    rightGroup,
  );
}

// Hidden on terminal states — a static fraction adds no information.
function renderInlineProgressBar(
  done: number,
  total: number,
  live: boolean,
): m.Children {
  if (!live) return null;
  if (total <= 0) return null;
  const pct = Math.max(0, Math.min(100, (done / total) * 100));
  return m(
    'span.pf-query-page__inline-progress',
    m('span.pf-query-page__inline-progress-fill', {
      style: {width: `${pct}%`},
    }),
  );
}

async function refreshAsyncStatus(tab: BigTraceEditorTab): Promise<void> {
  if (!tab.queryUuid) return;
  try {
    const status = await tab.queryClient?.getStatus(
      tab.queryUuid,
      tab.lifecycle.signal,
    );
    if (status) {
      queryStore.update(tab.queryUuid, {
        processedRows: status.processedRows ?? 0,
        processedTraces: status.processedTraces ?? 0,
        totalTraces: status.totalTraces ?? 0,
        status: status.status ?? 'N/A',
      });
    }
  } catch (e) {
    console.error('Failed to fetch query status on refresh:', e);
  }
  if (tab.dataSource instanceof BigtraceAsyncDataSource) {
    tab.dataSource.refresh();
    tab.lastProcessedRows = tab.execution?.processedRows ?? 0;
  }
  m.redraw();
}

// ---------------------------------------------------------------------------
// Result grid (Table tab) + Error tab + Chart placeholder.
// ---------------------------------------------------------------------------

function renderErrorTab(tab: BigTraceEditorTab): m.Children {
  const errorStr = tab.queryResult?.error ?? '';
  const fullText = errorStr
    .replaceAll('\\n', '\n')
    .replaceAll('\\t', '  ')
    .replaceAll('\\u003e', '>');
  return m('pre.pf-query-page__error-content', fullText);
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
            title: m('span.pf-query-page__error-tab-title', 'Error'),
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

  return m('.pf-query-page__results', [
    m(Tabs, {
      tabs,
      activeTabKey: activeTab,
      onTabChange: (key) => {
        tab.resultsTabKey = key;
      },
    }),
  ]);
}

function renderResultsGrid(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  runner: QueryRunner,
): m.Children {
  const queryResult = tab.queryResult!;
  const dataSource = tab.dataSource!;

  const isInitialLoad =
    tab.queryUuid !== undefined &&
    tab.queryUuid !== '' &&
    (tab.execution === undefined || tab.execution.status === 'UNKNOWN');
  if (isInitialLoad) {
    return m(
      EmptyState,
      {
        title: 'Loading query status...',
        icon: 'hourglass_empty',
        fillHeight: true,
      },
      m(Spinner),
    );
  }

  // Caller gated on hasRowsToShow, so partial-success failures
  // (quota cut-off, mid-stream sqlite error) still render the grid.
  const isTerminal =
    tab.execution?.status !== undefined &&
    TERMINAL_STATUSES.has(tab.execution.status);

  const tableContent: m.Children[] = [];

  // Sync uses static columns; async fills in once schema arrives.
  let columns = queryResult.columns;
  if (columns.length === 0 && dataSource instanceof BigtraceAsyncDataSource) {
    columns = dataSource.getColumns() ?? [];
  }

  if (dataSource instanceof BigtraceAsyncDataSource) {
    const error = dataSource.getError();
    // Suppress mid-stream 400s — those are the backend's transient
    // FAILED_PRECONDITION ("no rows yet"). Surface anything terminal.
    if (
      error !== null &&
      error !== '' &&
      (isTerminal || error.includes('status: 400') === false)
    ) {
      tableContent.push(
        m(EmptyState, {
          title: `Failed to load schema: ${error}`,
          icon: 'error',
          fillHeight: true,
        }),
      );
      return tableContent;
    }
  }

  if (columns.length === 0) {
    // Async mid-flight: kick `useRows` so the data source starts
    // fetching, then spin. (Sync re-opens intercepted upstream.)
    dataSource.useRows({mode: 'flat', columns: []});
    tableContent.push(
      m(
        EmptyState,
        {
          title: 'Loading schema...',
          icon: 'hourglass_empty',
          fillHeight: true,
        },
        m(Spinner),
      ),
    );
    return tableContent;
  }

  tableContent.push(
    renderDataGrid(tab, tabsState, runner, columns, queryResult, dataSource),
  );
  return tableContent;
}

function renderDataGrid(
  tab: BigTraceEditorTab,
  _tabsState: QueryTabsState,
  _runner: QueryRunner,
  columns: ReadonlyArray<string>,
  queryResult: QueryResponse,
  dataSource: DataSource,
): m.Children {
  const querySettings: SettingFilter[] = tab.querySettings;

  const columnSchema: ColumnSchema = {};
  for (const column of columns) {
    if (column === 'link') {
      columnSchema[column] = {
        cellRenderer: (value) => {
          if (value === null || value === undefined) return '';
          return linkify(String(value));
        },
      };
    } else {
      columnSchema[column] = {cellRenderer: undefined};
    }
  }
  const schema: SchemaRegistry = {data: columnSchema};

  return m(DataGrid, {
    schema,
    rootSchema: 'data',
    enablePivotControls: false, // In-memory datasource doesn't support pivoting.
    initialColumns: columns
      .filter((col) => {
        if (!col.startsWith('_')) return true;
        if (col === '_trace_id') return true;
        const settingId = col.substring(1);
        return querySettings.some(
          (s) => s.settingId === settingId && s.category === 'TRACE_METADATA',
        );
      })
      .map((col) => ({id: col, field: col})),
    className: 'pf-query-page__results',
    data: dataSource,
    // Without this, the entire results panel scrolls instead of just
    // the grid body — toolbar and sticky header detach.
    fillHeight: true,
    showExportButton: true,
    emptyStateMessage: 'Query returned no rows',
    toolbarItemsLeft: [
      m(
        'span.pf-query-page__results-summary',
        renderResultsSummary(tab, queryResult),
      ),
    ],
    toolbarItemsRight: [
      m(CopyToClipboardButton, {
        textToCopy: queryResult.query,
        title: 'Copy executed query to clipboard',
        label: 'Copy Query',
      }),
    ],
  });
}

// No "Showing X–Y" — the loaded window is a prefetch buffer, not the viewport.
function renderResultsSummary(
  tab: BigTraceEditorTab,
  queryResult: QueryResponse,
): string {
  if (!tab.materialize) {
    const durationStr = formatDurationS(Math.max(0, queryResult.durationMs));
    return `Returned ${queryResult.totalRowCount.toLocaleString()} rows in ${durationStr}`;
  }
  // Prefer post-filter count; fall back to live progress pre-first-fetch.
  const asyncDs =
    tab.dataSource instanceof BigtraceAsyncDataSource
      ? tab.dataSource
      : undefined;
  const count = asyncDs?.filteredTotalRows ?? tab.execution?.processedRows ?? 0;
  const isTerminal =
    tab.execution?.status !== undefined &&
    TERMINAL_STATUSES.has(tab.execution.status);
  const text = `${count.toLocaleString()} rows`;
  return isTerminal ? text : `${text} · running…`;
}

// ---------------------------------------------------------------------------
// Lazily build the async data source for tabs restored from localStorage.
// ---------------------------------------------------------------------------

function attachAsyncDataSource(
  tab: BigTraceEditorTab,
  runner: QueryRunner,
): void {
  if (!tab.queryUuid) return;
  const endpointSetting = endpointStorage.get('bigtraceEndpoint');
  const endpoint = endpointSetting ? (endpointSetting.get() as string) : '';
  const queryClient = new BigtraceQueryClient(endpoint);
  tab.queryClient = queryClient;
  // Sync isn't persisted; empty source → "re-run to see results" hint.
  if (!tab.materialize) {
    tab.dataSource = new InMemoryDataSource([]);
    return;
  }
  tab.dataSource = new BigtraceAsyncDataSource(
    tab.queryUuid,
    queryClient,
    () => tab.execution?.processedRows ?? 0,
    tab.lifecycle.signal,
  );
  tab.isLoading = true;
  runner.startPolling(tab);

  if (tab.queryResult === undefined) {
    tab.queryResult = {
      rows: [],
      columns: [],
      error: undefined,
      totalRowCount: 0,
      durationMs: 0,
      statementWithOutputCount: 0,
      statementCount: 1,
      lastStatementSql: tab.editorText,
      query: tab.editorText,
    };
  }
}
