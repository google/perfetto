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
import {Icon} from '../../widgets/icon';
import {linkify} from '../../widgets/anchor';
import {Spinner} from '../../widgets/spinner';
import {SplitPanel} from '../../widgets/split_panel';
import {Stack, StackAuto} from '../../widgets/stack';
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
import {queryStore, TERMINAL_STATUSES} from '../query/query_store';
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

// Renders one editor tab: toolbar + Codemirror editor + status box + the
// tabular/chart result panel. State for the tab lives on `tab`; this view
// is purely presentational and delegates side-effects to the QueryRunner
// and QueryTabsState passed in attrs.
export class EditorTabView implements m.ClassComponent<EditorTabViewAttrs> {
  view({attrs}: m.Vnode<EditorTabViewAttrs>): m.Children {
    const {tab, tabsState, runner, useBigtraceBackend} = attrs;

    // For tabs reopened from history, lazily wire up the dataSource on
    // first render. The runner picks up polling from there.
    if (tab.queryUuid && !tab.dataSource) {
      attachAsyncDataSource(tab, runner);
    }

    if (tab.dataSource && tab.queryResult && tab.materialize && tab.execution) {
      // Keep the tab's "totalRowCount" in sync with the live query store
      // so the results-summary text stays accurate.
      tab.queryResult.totalRowCount = tab.execution.processedRows;
    }

    return m(SplitPanel, {
      direction: 'vertical',
      // Most BigTrace queries are short; default the editor to ~22% and
      // let the user drag down for more space.
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
              // Disable when nothing executable remains
              // (whitespace + SQL line comments).
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
          // Progress feedback for running queries lives in
          // renderProgressBar() above the results panel — full-width,
          // determinate when totalTraces is known. The toolbar slot
          // here previously held a tiny indeterminate bar that was
          // easy to miss.
          m(Switch, {
            label: 'Persistent',
            title:
              'ON: results saved to History (Persistent tab) — reopen later. ' +
              'OFF: results shown inline and discarded when the tab closes.',
            checked: tab.materialize,
            // Mode is captured at submit time; disable mid-run so
            // the toggle isn't a false-affordance.
            disabled: tab.isLoading,
            onchange: (e: Event) => {
              tab.materialize = (e.target as HTMLInputElement).checked;
              setHistoryActiveTab(tab.materialize);
              tabsState.markDirty();
            },
          }),
          // Vertical divider — Materialize is a mode switch (changes how
          // Run behaves), Result limit is a numeric param. They affect
          // the next run differently; the rule makes that visual.
          m('span.pf-query-page__toolbar-divider', {'aria-hidden': 'true'}),
          m('span', 'Limit:'),
          m(TextInput, {
            type: 'number',
            value: String(tab.limit),
            placeholder: 'Limit',
            // Captured at submit time; disable mid-run.
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
      // Swallow Ctrl/Cmd+S so the browser's "Save Page As…" doesn't
      // open. Auto-save already runs on every keystroke; the handler
      // is just here to claim the keybinding.
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

// Round to the nearest second for display. "<1s" for sub-500ms runs
// so the user sees the query actually ran.
function formatDurationS(ms: number): string {
  if (ms < 500) return '<1s';
  return Duration.format(Duration.fromMillis(Math.round(ms / 1000) * 1000));
}

// "Running query…" with a live elapsed-time readout. Owns its own
// setInterval since sync queries don't drive periodic redraws.
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
// Results panel: status box (async only) + error banner + DataGrid/Chart.
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

  const errorBanner = renderErrorBanner(tab);
  const processedRows = tab.execution?.processedRows ?? 0;

  // Sync (Ephemeral) re-open from history with no re-run yet:
  // results aren't persisted server-side, and `processedRows` is
  // intentionally 0 (the backend doesn't track row counts for sync
  // queries — it'd be misleading metadata). Show an explicit hint
  // rather than fall into the `processedRows == 0` "Query returned
  // no rows" branch, which would imply the original run had no
  // rows. The discriminator is `tab.queryResult.rows.length`:
  // resumeFromHistory leaves it at 0; runSync repopulates it on
  // re-run. Errors fall through to the regular path so the banner
  // surfaces them.
  const isSyncReopenNoRerun =
    !tab.materialize &&
    Boolean(tab.queryUuid) &&
    tab.queryResult.rows.length === 0 &&
    !tab.queryResult.error;

  // Async tab whose materialized table is gone (TTL-expired,
  // CANCELLED-with-zero-rows, or post-failure cleanup), but the
  // metadata row still exists with `processedRows > 0`. Without
  // this branch the editor enters renderResultsGrid → "Loading
  // schema..." → triggers a dummy useRows (no pagination) → the
  // data source's `needsInitial` gate requires `limit > 0` so no
  // fetch fires → spinner never resolves. Detect upfront and show
  // a recovery hint. Errors fall through (the error banner shows
  // them).
  // Only treat as "cleared" once terminal — mid-run, the table is
  // still being built and "Results no longer available" misleads.
  const isTerminalStatus =
    tab.execution?.status !== undefined &&
    TERMINAL_STATUSES.has(tab.execution.status);
  const isAsyncTableCleared =
    tab.materialize &&
    Boolean(tab.queryUuid) &&
    !tab.execution?.tableName &&
    !tab.queryResult.error &&
    isTerminalStatus;

  // Whether to render the grid. For async, gate on the live
  // processedRows from the materialized table; for sync (whose
  // `processedRows` stays at 0 server-side), gate on the inline
  // rows actually held in tab.queryResult.
  const hasRowsToShow = tab.materialize
    ? processedRows > 0
    : tab.queryResult.rows.length > 0;

  return m(
    '.pf-query-page__results-panel',
    status,
    m('.pf-query-page__results-container', [
      errorBanner,
      isSyncReopenNoRerun
        ? m(EmptyState, {
            title: 'Re-run the query to see results',
            icon: 'refresh',
            fillHeight: true,
          })
        : isAsyncTableCleared
          ? m(EmptyState, {
              // CANCELLED = user-driven; generic message covers TTL
              // expiry / post-failure cleanup.
              title:
                tab.execution?.status === 'CANCELLED'
                  ? 'Query was cancelled'
                  : 'Results no longer available',
              icon: 'refresh',
              fillHeight: true,
            })
          : hasRowsToShow
            ? renderResultsGrid(tab, tabsState, runner)
            : tab.isLoading
              ? m('div')
              : !tab.queryResult.error &&
                m(EmptyState, {
                  title: 'Query returned no rows',
                  icon: 'search',
                  fillHeight: true,
                }),
    ]),
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

  return m(
    Box,
    {className: 'pf-query-page__status-bar'},
    m(
      Stack,
      {orientation: 'horizontal', gap: '16px', alignItems: 'center'},
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
        // Wire value uses underscores ("IN_PROGRESS"); display swaps
        // them for spaces so the pill reads naturally. The transient
        // "UNKNOWN" state (pre-first-poll) shows as "STARTING" so the
        // pill matches the body's "Loading query status…" copy.
        status === 'UNKNOWN' ? 'STARTING' : status.replace(/_/g, ' '),
      ),
      // Duration leads the stats with its own dedicated chip: a clock
      // icon + larger, monospaced value. It's the metric users care
      // about most (how long has this been running / how long did it
      // take), and as a plain label/value pair it was too easy to miss.
      m(
        'span.pf-query-page__status-bar-duration',
        {
          title: !isTerminal ? 'Elapsed time (live)' : 'Total query duration',
        },
        m(Icon, {
          icon: 'schedule',
          className: 'pf-query-page__status-bar-duration-icon',
        }),
        m('span.pf-query-page__status-bar-duration-value', durationStr),
      ),
      m('span.pf-query-page__toolbar-divider', {'aria-hidden': 'true'}),
      // Traces: denominator is firm; the numerator lags the 3s poll
      // and is dimmed while live.
      m(
        'span.pf-query-page__status-bar-stat',
        m('span.pf-query-page__status-bar-stat-label', 'Traces'),
        m(
          'span.pf-query-page__status-bar-stat-value',
          {
            title: !isTerminal
              ? 'Numerator updates on the next poll (≤3s lag); denominator is exact.'
              : undefined,
          },
          m(
            'span',
            {
              className: !isTerminal
                ? 'pf-query-page__status-bar-live'
                : undefined,
            },
            String(processedTraces),
          ),
          '/',
          String(totalTraces),
        ),
        renderInlineProgressBar(processedTraces, totalTraces, !isTerminal),
      ),
      m('span.pf-query-page__toolbar-divider', {'aria-hidden': 'true'}),
      m(
        'span.pf-query-page__status-bar-stat',
        {
          className:
            processedRows === 0
              ? 'pf-query-page__status-bar-stat--empty'
              : undefined,
        },
        m('span.pf-query-page__status-bar-stat-label', 'Rows'),
        m(
          'span.pf-query-page__status-bar-stat-value',
          {
            title: `${processedRows.toLocaleString()} of result limit ${tab.limit.toLocaleString()}`,
          },
          processedRows.toLocaleString(),
        ),
        // Denominator here is the user-set result limit (a soft cap, not
        // a target). The bar fills as the query approaches the limit;
        // for queries that naturally produce fewer rows, it stays low —
        // that's expected, the limit is informational.
        renderInlineProgressBar(processedRows, tab.limit, !isTerminal),
      ),
    ),
  );
}

// Inline mini progress bar shown after the N/M numbers in the status
// bar. Only rendered while the query is running — once the query has
// reached a terminal state (SUCCESS / FAILED / CANCELLED) the bar is
// hidden, since it would just be a static fraction.
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

// TP-style errors come back wrapped in a SQLite-flavored traceback:
//
//   [trace_name] Traceback (most recent call last):
//     File "stdin" line 1 col 1
//       SELECT ...
//       ^
//   no such table: foo
//
// The last non-empty line is the user-actionable message; everything above
// is sqlite scaffolding. For non-TP errors (server quotas, network errors,
// short single-line messages) the headline stays equal to the input and the
// expandable details simply repeats it — which is fine.
function extractErrorHeadline(errorStr: string): string {
  const normalized = errorStr.replaceAll('\\n', '\n');
  const lines = normalized.split('\n');
  for (let i = lines.length - 1; i >= 0; i--) {
    const trimmed = lines[i].trim();
    if (trimmed !== '' && trimmed !== '^') return trimmed;
  }
  return errorStr;
}

// Strip leaked transport/status prefixes from error headlines:
//   "HTTP error! status: 400, message: <real msg>"
//   "INVALID_ARGUMENT: <real msg>"
//   "status: 400 detail: NOT_FOUND <real msg>"
const _GRPC_STATUS_RE =
  /^\s*(?:status\s*:\s*\d+\s*[,:]?\s*)?(?:detail\s*:\s*)?(?:OK|CANCELLED|UNKNOWN|INVALID_ARGUMENT|DEADLINE_EXCEEDED|NOT_FOUND|ALREADY_EXISTS|PERMISSION_DENIED|RESOURCE_EXHAUSTED|FAILED_PRECONDITION|ABORTED|OUT_OF_RANGE|UNIMPLEMENTED|INTERNAL|UNAVAILABLE|DATA_LOSS|UNAUTHENTICATED)\s*[:\-]?\s*/i;
const _HTTP_ERROR_RE =
  /^\s*HTTP error!\s*status\s*:\s*\d+\s*,\s*message\s*:\s*/i;
function stripGrpcStatus(text: string): string {
  let stripped = text.replace(_HTTP_ERROR_RE, '');
  stripped = stripped.replace(_GRPC_STATUS_RE, '').trim();
  // Fall back to original if stripping consumed everything.
  return stripped.length === 0 ? text : stripped;
}

function renderErrorBanner(tab: BigTraceEditorTab): m.Children {
  const errorStr = tab.queryResult?.error;
  if (errorStr === undefined) return false;

  const isPreconditionFailure =
    errorStr.includes('FAILED_PRECONDITION') ||
    errorStr.includes('failed_precondition');
  const displayTitle = isPreconditionFailure
    ? 'Results no longer available'
    : 'Query failed';
  const headline = isPreconditionFailure
    ? 'The persistent results table for this query has expired. You may need to ' +
      'run the query again.'
    : stripGrpcStatus(extractErrorHeadline(errorStr));
  const fullText = errorStr
    .replaceAll('\\n', '\n')
    .replaceAll('\\t', '  ')
    .replaceAll('\\u003e', '>');
  const showDetailsToggle = fullText.trim() !== headline.trim();

  return m(
    '.pf-results-table__error',
    m(Icon, {
      className: 'pf-results-table__error-icon',
      icon: 'error',
      intent: Intent.Danger,
    }),
    m('.pf-results-table__error-body', [
      m('.pf-results-table__error-title', displayTitle),
      // Headline always visible; full traceback collapsed in <details>.
      m('.pf-results-table__error-headline', headline),
      showDetailsToggle &&
        m(
          'details',
          m(
            'summary',
            {style: {cursor: 'pointer', opacity: 0.7, fontSize: '0.85em'}},
            'Show full error',
          ),
          m(
            'pre.pf-results-table__error-message',
            {
              style: {
                overflow: 'auto',
                maxWidth: '100%',
                textAlign: 'left',
                marginTop: '6px',
                fontSize: '0.85em',
                opacity: 0.8,
              },
            },
            fullText,
          ),
        ),
    ]),
  );
}

// ---------------------------------------------------------------------------
// Result grid (Table tab) + Chart placeholder.
// ---------------------------------------------------------------------------

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

  // Failures may still have committed partial rows — e.g. a server-side
  // quota cut-off after streaming N results, or a sqlite error mid-stream.
  // The renderResultsPanel caller has already gated on `processedRows > 0`,
  // so reaching here means we have data; render it alongside the error
  // banner instead of dropping it on the floor.

  const isTerminal =
    tab.execution?.status !== undefined &&
    TERMINAL_STATUSES.has(tab.execution.status);

  const tableContent: m.Children[] = [];

  // Heuristic statement count by `;` splitting — the runner doesn't
  // populate queryResult.statementCount.
  const statementCount = queryResult.query
    .split(';')
    .map((s) => s.trim())
    .filter((s) => s.length > 0).length;
  if (statementCount > 1) {
    tableContent.push(
      m(Box, [
        m(
          Callout,
          {icon: 'warning', intent: Intent.None},
          'Only the results from the last statement are displayed.',
        ),
      ]),
    );
  }

  // Resolve columns: prefer the result's static columns (sync queries);
  // fall back to the async data source's snapshot once schema arrives.
  let columns = queryResult.columns;
  if (columns.length === 0 && dataSource instanceof BigtraceAsyncDataSource) {
    columns = dataSource.getColumns() ?? [];
  }

  if (dataSource instanceof BigtraceAsyncDataSource) {
    const error = dataSource.getError();
    // Show errors when the query is terminal (real failures) or when
    // the error isn't a 400 (which during streaming is the backend's
    // FAILED_PRECONDITION for "no rows yet" / "table not yet
    // materialized" — transient by definition until the query
    // finishes, so suppress to avoid flashing an error banner).
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
      return wrapInTabs(tableContent);
    }
  }

  if (columns.length === 0) {
    // Async query mid-flight: schema hasn't arrived yet. Trigger
    // `useRows` so the data source starts fetching, then show a
    // spinner. (Sync re-opens are intercepted upstream in
    // renderResultsPanel by the `isSyncReopenNoRerun` branch, so
    // they never reach here.)
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
    return wrapInTabs(tableContent);
  }

  tableContent.push(
    renderDataGrid(tab, tabsState, runner, columns, queryResult, dataSource),
  );
  return wrapInTabs(tableContent);
}

function wrapInTabs(tableContent: m.Children): m.Children {
  return m('.pf-query-page__results', [
    m(Tabs, {
      tabs: [
        {key: 'table', title: 'Table', content: tableContent},
        {
          key: 'chart',
          title: 'Chart',
          content: m(
            EmptyState,
            {
              title: 'Charts are coming soon',
              icon: 'bar_chart',
            },
            m(
              'div',
              {
                style: {
                  marginTop: '8px',
                  opacity: 0.7,
                  maxWidth: '420px',
                  textAlign: 'center',
                  lineHeight: '1.4',
                },
              },
              "Run a query that returns numeric columns and you'll be " +
                'able to plot the results here.',
            ),
          ),
        },
      ],
    }),
  ]);
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
    // Without fillHeight, the table renders at its intrinsic content
    // height and the whole results panel scrolls (toolbar, headers,
    // body all together). With it, the inner Grid takes 100% of its
    // parent and only the body scrolls, keeping the toolbar +
    // sticky column header anchored.
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

// Result-summary text shown in the toolbar's left slot.
//
// Sync queries: rowcount + duration on a single line.
// Materialized queries: just the total. We deliberately don't render
// a "Showing X-Y" range — the DataGrid widget virtualizes; the
// "loaded window" the data source holds is a prefetch buffer (typically
// viewport + ~80 rows above and below), not what the user actually
// sees. Putting that range in the toolbar misleads more than it helps.
// The Grid's scrollbar already shows the user's position.
function renderResultsSummary(
  tab: BigTraceEditorTab,
  queryResult: QueryResponse,
): string {
  if (!tab.materialize) {
    const durationStr = formatDurationS(Math.max(0, queryResult.durationMs));
    return `Returned ${queryResult.totalRowCount.toLocaleString()} rows in ${durationStr}`;
  }
  // Prefer the post-filter count so the toolbar matches the Grid;
  // fall back to live progress before the first fetch lands.
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
// Side effect: lazily build the async data source when a tab is restored
// from localStorage with a queryUuid but without a live dataSource.
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
  // Sync (Ephemeral) queries aren't persisted server-side — there's no
  // materialized table to fetch from. Attach an empty in-memory data
  // source so the result panel can render a cleaner "re-run to see
  // results" empty-state instead of trying (and failing) to load a
  // schema. Skip polling too: the QueryStore already has the
  // history-row metadata (sql, timing, processedRows).
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
