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
import {
  perfettoSqlCompletions,
  perfettoSqlDiagnostics,
} from '../query/sql_completion';
import {
  onSqlEngineReady,
  onSqlSchemaApplied,
} from '../../components/sql_intelligence';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {SplitPanel} from '../../widgets/split_panel';
import {Stack, StackAuto} from '../../widgets/stack';
import {Switch} from '../../widgets/switch';
import {TextInput} from '../../widgets/text_input';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';
import {BigtraceAsyncDataSource} from '../query/bigtrace_async_data_source';
import {setHistoryActiveTab} from '../query/query_history';
import {formatPerfettoSql} from '../query/sql_formatter';
import {BigtraceQueryClient} from '../query/bigtrace_query_client';
import type {QueryRunner} from '../query/query_runner';
import {
  type BigTraceEditorTab,
  type QueryTabsState,
  deriveTitleFromQuery,
  effectiveTabSettings,
} from './query_tabs_state';
import {renderResultsPanel} from './results_panel';
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

    return m('.pf-bt-editor-tab', [
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
// Per-tab bindings shared between the chip strip and any modal it opens.
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
    getSql: () => tab.editorText,
    setQueryAndTitle: (perfettoSql, title) => {
      tab.editorText = perfettoSql;
      // A title other than "Query N" sticks — maybeAutoNameTab won't replace it.
      if (title) tab.title = title;
      tabsState.markDirty();
      m.redraw();
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

// ---------------------------------------------------------------------------
// Editor panel: toolbar (Run/Cancel + limit + Persistent) and the editor.
// ---------------------------------------------------------------------------

function renderEditorPanel(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  runner: QueryRunner,
  useBigtraceBackend: boolean,
): m.Children {
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
              label: 'Run Query',
              icon: 'play_arrow',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
              disabled: deriveTitleFromQuery(tab.editorText) === undefined,
              onclick: () => {
                setHistoryActiveTab(tab.materialize);
                tabsState.maybeAutoNameTab(tab.id, tab.editorText);
                runner.run(tab, tab.editorText);
              },
            }),
        m(
          Stack,
          {orientation: 'horizontal', className: 'pf-bt-query-page__hotkeys'},
          'or press',
          m(HotkeyGlyphs, {hotkey: 'Mod+Enter'}),
        ),
        m(StackAuto),
        // Icon-only to keep the toolbar lean; the editor binds the same chord.
        m(Button, {
          icon: 'format_align_left',
          title: 'Format query (Alt+Shift+F)',
          disabled: deriveTitleFromQuery(tab.editorText) === undefined,
          onclick: () => void formatTabQuery(tab, tabsState, tab.editorText),
        }),
        useBigtraceBackend && [
          m('span.pf-bt-toolbar-divider', {'aria-hidden': 'true'}),
          m(Switch, {
            label: 'Persistent',
            title:
              'ON: results saved to History (Persistent tab) — reopen later. ' +
              'OFF: results shown inline and discarded when the tab closes.',
            checked: tab.materialize,
            disabled: tab.isLoading,
            onchange: (e: Event) => {
              tab.materialize = (e.target as HTMLInputElement).checked;
              setHistoryActiveTab(tab.materialize);
              tabsState.markDirty();
            },
          }),
          m('span.pf-bt-toolbar-divider', {'aria-hidden': 'true'}),
          m('span', 'Limit:'),
          m(TextInput, {
            type: 'number',
            value: String(tab.limit),
            placeholder: 'Limit',
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
      completions: perfettoSqlCompletions,
      diagnostics: perfettoSqlDiagnostics,
      onDiagnosticsRefresh: (refresh: () => void) => {
        onSqlEngineReady(refresh);
        onSqlSchemaApplied(refresh);
      },
      onSave: () => {},
      // Alt+Shift+F (option+shift+F on Mac); listed in the help modal.
      onFormat: (text: string) => formatTabQuery(tab, tabsState, text),
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
