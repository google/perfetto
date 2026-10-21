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
import {Editor} from '../../widgets/editor';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {Spinner} from '../../widgets/spinner';
import {SplitPanel} from '../../widgets/split_panel';
import {Stack, StackAuto} from '../../widgets/stack';
import {Switch} from '../../widgets/switch';
import {TabStrip} from '../../widgets/tab_strip';
import {TextInput} from '../../widgets/text_input';
import {showModal} from '../../widgets/modal';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';
import {
  serverHasFeature,
  serverLacksFeature,
} from '../settings/server_capabilities';
import {BigtraceAsyncDataSource} from '../query/bigtrace_async_data_source';
import {formatPerfettoSql} from '../query/sql_formatter';
import {BigtraceQueryClient} from '../query/bigtrace_query_client';
import type {QueryRunner} from '../query/query_runner';
import {
  type BigTraceEditorTab,
  type QueryTabsState,
  addQuery,
  applyModeDefaults,
  canRenameQuery,
  hasQueryText,
  effectiveTabSettings,
  openEngineTab,
  removeQuery,
} from './query_tabs_state';
import {openQuerySettingsModal} from './query_settings_modal';
import {renderResultsPanel} from './results_panel';
import {QueryLauncher} from './query_launcher';
import type {SettingCategory, SettingFilter} from '../settings/settings_types';
import type {SettingsBindings} from '../settings/tab_bound_setting';
import {BigtraceSettingsBar} from './bigtrace_settings_bar';

export interface EditorTabViewAttrs {
  readonly tab: BigTraceEditorTab;
  readonly tabsState: QueryTabsState;
  readonly runner: QueryRunner;
  readonly useBigtraceBackend: boolean;
}

// Split pane with editor on top, results on bottom.
// Rendering lives in results_panel.ts and status_box.ts.
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

    // A tab with no configuration yet shows the launcher instead of the editor:
    // every query starts from a preset or a deliberate custom setup. A
    // configured tab shows it too while its Settings are open. DuckDB tabs
    // have no trace selection.
    const isDuckdb = tab.queryEngine === 'duckdb';
    if (!isDuckdb && (!tab.configured || tab.settingsSession !== undefined)) {
      return m(
        '.pf-bt-editor-tab',
        m(QueryLauncher, {
          tab,
          tabsState,
          bindings: buildTabBindings(tab, tabsState),
        }),
      );
    }

    return m('.pf-bt-editor-tab', [
      !isDuckdb &&
        m(BigtraceSettingsBar, {
          tab,
          tabsState,
          bindings: buildTabBindings(tab, tabsState),
        }),
      m(SplitPanel, {
        direction: 'vertical',
        initialSplit: {percent: 22},
        minSize: 100,
        firstPanel: renderEditorPanel(
          tab,
          tabsState,
          runner,
          useBigtraceBackend,
        ),
        secondPanel: renderResultsPanel(tab, tabsState),
      }),
    ]);
  }
}

// ---------------------------------------------------------------------------
// Per-tab bindings shared between the chip strip and the settings form.
// Getters read live; setters mutate in place and mark dirty.
// getEffectiveSettings layers per-tab overrides over global defaults so
// /trace_metadata sees a complete settings array even before the user edits.
// ---------------------------------------------------------------------------

function buildTabBindings(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
): SettingsBindings {
  return {
    getEffectiveSettings: () => effectiveTabSettings(tab),
    getSettingValue: (id) => {
      const entry = tab.querySettings.find((s) => s.settingId === id);
      return entry?.values;
    },
    setSettingValue: (id, values, category) => {
      const next = [...tab.querySettings];
      const idx = next.findIndex((s) => s.settingId === id);
      const entry: SettingFilter = {
        settingId: id,
        values: [...values],
        category: category as SettingCategory,
      };
      if (idx >= 0) next[idx] = entry;
      else next.push(entry);
      tab.querySettings = next;
      tabsState.markDirty();
    },
    getTraceFilters: () => tab.traceFilters,
    setTraceFilters: (filters) => {
      tab.traceFilters = [...filters];
      tabsState.markDirty();
    },
    getTraceMetadataColumns: () => tab.traceMetadataColumns,
    setTraceMetadataColumns: (cols) => {
      tab.traceMetadataColumns = cols === null ? null : [...cols];
      tabsState.markDirty();
    },
    getTraceOrderBy: () => tab.traceOrderBy,
    setTraceOrderBy: (orderBy) => {
      tab.traceOrderBy = orderBy;
      tabsState.markDirty();
    },
    isSettingDisabled: (id) => tab.disabledSettings.includes(id),
    setSettingDisabled: (id, disabled) => {
      const set = new Set(tab.disabledSettings);
      if (disabled) set.add(id);
      else set.delete(id);
      tab.disabledSettings = [...set];
      tabsState.markDirty();
    },
    getRowLimit: () => tab.limit,
    setRowLimit: (limit) => {
      tab.limit = limit;
      tabsState.markDirty();
    },
    getMaterialize: () => tab.materialize,
    setMaterialize: (materialize) => {
      // Same path as the toolbar switch: caps the user never touched follow
      // the mode.
      applyModeDefaults(tab, materialize);
      tabsState.markDirty();
    },
    getExperimentFilter: () => tab.experimentFilter,
    setExperimentFilter: (filter) => {
      tab.experimentFilter = filter;
      tabsState.markDirty();
    },
    getDateFilter: () => tab.dateFilter,
    setDateFilter: (filter) => {
      tab.dateFilter = filter;
      tabsState.markDirty();
    },
    getTraceLimit: () => tab.traceLimit,
    setTraceLimit: (limit) => {
      if (limit >= 0) {
        tab.traceLimit = limit;
        tabsState.markDirty();
      }
    },
    getTableTtlDays: () => tab.tableTtlDays,
    setTableTtlDays: (days) => {
      if (days > 0) {
        tab.tableTtlDays = days;
        tabsState.markDirty();
      }
    },
  };
}

// Format a tab's query in place; no-op when formatting fails (the error is
// logged) or the text is already clean. Bound to Alt+Shift+F in the editor.
async function formatTabQuery(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  text: string,
): Promise<void> {
  const formatted = await formatPerfettoSql(text);
  if (formatted === undefined || formatted === tab.editorText) return;
  tab.editorText = formatted;
  tabsState.markDirty();
  m.redraw();
}

// Runs `tab`, first asking the backend whether a result table a persistent
// run would write already exists. If one does, the user picks between
// overwriting it and running a copy of the tab, under another name, in a new
// tab.
async function runTab(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  runner: QueryRunner,
  query: string,
): Promise<void> {
  const endpoint = getBigtraceEndpoint().trim();
  if (tab.materialize && endpoint !== '') {
    const client = new BigtraceQueryClient(endpoint);
    for (const [i, q] of tab.queries.entries()) {
      if (!hasQueryText(q.sql)) continue;
      let check;
      try {
        check = await client.checkTableExists(
          q.tableName,
          tab.lifecycle.signal,
        );
      } catch {
        continue; // The backend has the last word when the run starts.
      }
      if (!check.exists) continue;
      const choice = await askOverwrite(
        check.resolvedTableName,
        check.suggestedTableName ?? `${check.resolvedTableName}_1`,
      );
      if (choice === undefined) return;
      if (choice.newTableName === undefined) break;
      const clone = tabsState.cloneTab(tab.id, choice.newTableName, i);
      if (clone !== undefined) await runner.run(clone, query);
      return;
    }
  }
  await runner.run(tab, query);
}

// Undefined: cancelled. No `newTableName`: overwrite.
function askOverwrite(
  table: string,
  suggested: string,
): Promise<{readonly newTableName?: string} | undefined> {
  return new Promise((resolve) => {
    let name = suggested;
    void showModal({
      title: `Table "${table}" already exists`,
      content: () =>
        m(
          Stack,
          {orientation: 'vertical', gap: 'normal'},
          'Overwrite it, or run in a new tab with another table name:',
          m(TextInput, {
            value: name,
            autofocus: true,
            onInput: (value: string) => (name = value),
          }),
        ),
      buttons: [
        {text: 'Cancel', action: () => resolve(undefined)},
        {text: 'Overwrite', action: () => resolve({})},
        {
          text: 'Create New Tab',
          primary: true,
          action: () => resolve({newTableName: name.trim() || suggested}),
        },
      ],
    }).then(() => resolve(undefined));
  });
}

// ---------------------------------------------------------------------------
// Editor panel: toolbar (Run/Cancel + limit + Persistent) and the editor.
// ---------------------------------------------------------------------------

function renderEditorPanel(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  runner: QueryRunner,
  useBigtraceBackend: boolean,
): m.Children {
  const multi = tab.queries.length > 1;
  const unsupported = unsupportedByServer(tab);
  return m('.pf-bt-query-page__editor-panel', [
    m(Box, {className: 'pf-bt-query-page__toolbar'}, [
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
              label: multi ? `Run ${tab.queries.length} Queries` : 'Run Query',
              icon: 'play_arrow',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
              disabled:
                unsupported !== undefined ||
                !tab.queries.some((q) => hasQueryText(q.sql)),
              onclick: () => runTab(tab, tabsState, runner, tab.editorText),
            }),
        m(
          Stack,
          {orientation: 'horizontal', className: 'pf-bt-query-page__hotkeys'},
          'or press',
          m(HotkeyGlyphs, {hotkey: 'Mod+Enter'}),
        ),
        m('span.pf-bt-divider', {'aria-hidden': 'true'}),
        // Icon-only to keep the toolbar lean; the editor binds the same chord.
        m(Button, {
          icon: 'format_align_left',
          title: 'Format query (Alt+Shift+F)',
          disabled: !hasQueryText(tab.editorText),
          onclick: () => void formatTabQuery(tab, tabsState, tab.editorText),
        }),
        m(StackAuto),
        useBigtraceBackend && renderRunControls(tab, tabsState),
      ]),
    ]),
    multi && renderQueryStrip(tab, tabsState),
    unsupported !== undefined &&
      m(
        Callout,
        {icon: 'info', intent: Intent.Warning},
        unsupported,
        tab.queryEngine === 'duckdb' &&
          m(Button, {
            label: 'New PerfettoSQL tab',
            icon: 'code',
            compact: true,
            onclick: () => openEngineTab(tabsState, 'traces'),
          }),
      ),
    tab.queryEngine === 'traces' &&
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
      onSave: () => {},
      // Alt+Shift+F (option+shift+F on Mac); listed in the help modal.
      onFormat: (text: string) => formatTabQuery(tab, tabsState, text),
      onUpdate: (text: string) => {
        tab.editorText = text;
        tabsState.markDirty();
      },
      onExecute: (query: string) => {
        if (unsupported === undefined) {
          void runTab(tab, tabsState, runner, query);
        }
      },
    }),
  ]);
}

// Why the configured server can't run `tab`, which a server with more
// features (the local server) left behind, or undefined if it can.
function unsupportedByServer(tab: BigTraceEditorTab): string | undefined {
  if (tab.queryEngine === 'duckdb' && serverLacksFeature('duckdb')) {
    return 'This server does not run DuckDB SQL.';
  }
  if (tab.queries.length > 1 && serverLacksFeature('multi_query')) {
    return (
      'This server runs one query at a time: remove the other queries to ' +
      'run this tab.'
    );
  }
  return undefined;
}

// The queries of a multi-query tab, each named after its result table
// (double-click to rename) and marked while running or failed.
function renderQueryStrip(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
): m.Children {
  return m(
    TabStrip,
    {variant: 'underline'},
    tab.queries.map((q, i) =>
      m(
        TabStrip.Tab,
        {
          active: i === tab.activeQueryIndex,
          icon:
            q.execution?.status === 'IN_PROGRESS'
              ? m(Spinner)
              : q.execution?.status === 'FAILED'
                ? 'error'
                : m('span', `#${i + 1}`),
          onclick: () => {
            tab.activeQueryIndex = i;
            tabsState.markDirty();
          },
          onRename: canRenameQuery(tab, q)
            ? (name: string) => {
                q.tableName = name;
                tabsState.markDirty();
              }
            : undefined,
          onClose: tab.isLoading
            ? undefined
            : () => {
                removeQuery(tab, i);
                tabsState.markDirty();
              },
        },
        q.tableName,
      ),
    ),
  );
}

// Mode switch, the row cap, and the gear opening the query-settings modal
// (the trace cap and every non-trace-selection setting). The gear is always
// there: the trace cap is a universal request field, whatever the backend
// declares. DuckDB tabs have only the mode switch.
function renderRunControls(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
): m.Children {
  const isTraces = tab.queryEngine === 'traces';
  return [
    isTraces &&
      serverHasFeature('multi_query') &&
      m(Button, {
        icon: 'add',
        label: 'Add Query',
        title: 'Run another query in the same pass over the traces',
        disabled: tab.isLoading,
        onclick: () => {
          addQuery(tab);
          tabsState.markDirty();
        },
      }),
    m(Switch, {
      label: 'Persistent',
      title:
        'ON: results saved to History — reopen later. ' +
        'OFF: results shown inline and discarded when the tab closes.',
      checked: tab.materialize,
      disabled: tab.isLoading,
      onchange: (e: Event) => {
        applyModeDefaults(tab, (e.target as HTMLInputElement).checked);
        tabsState.markDirty();
      },
    }),
    isTraces && [
      m('span.pf-bt-divider', {'aria-hidden': 'true'}),
      m('span', 'Rows:'),
      m(TextInput, {
        type: 'number',
        className: 'pf-bt-limit-input',
        value: String(tab.limit),
        title: 'Maximum rows this query returns.',
        disabled: tab.isLoading,
        onInput: (value: string) => {
          const newLimit = parseInt(value, 10);
          if (!isNaN(newLimit) && newLimit >= 0) {
            tab.limit = newLimit;
            tabsState.markDirty();
          }
        },
      }),
      m(Button, {
        icon: 'settings',
        title:
          'Advanced query settings — the trace cap and the query options. ' +
          'Applied from the next run.',
        disabled: tab.isLoading,
        onclick: () =>
          void openQuerySettingsModal(
            tab,
            tabsState,
            buildTabBindings(tab, tabsState),
          ),
      }),
    ],
  ];
}

// ---------------------------------------------------------------------------
// Lazily build the data source for tabs restored from localStorage.
// ---------------------------------------------------------------------------

function attachAsyncDataSource(
  tab: BigTraceEditorTab,
  runner: QueryRunner,
): void {
  if (!tab.queryUuid) return;
  const queryClient = new BigtraceQueryClient(getBigtraceEndpoint());
  tab.queryClient = queryClient;
  if (!tab.materialize) {
    tab.dataSource = new InMemoryDataSource([]);
    return;
  }
  tab.dataSource = new BigtraceAsyncDataSource(
    tab.queryUuid,
    queryClient,
    () => tab.execution?.processedRows ?? 0,
    tab.lifecycle.signal,
    () => tab.execution?.schema,
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
