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
import {Chip} from '../../widgets/chip';
import {Intent} from '../../widgets/common';
import {Editor} from '../../widgets/editor';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {SplitPanel} from '../../widgets/split_panel';
import {Stack, StackAuto} from '../../widgets/stack';
import {Switch} from '../../widgets/switch';
import {TextInput} from '../../widgets/text_input';
import {showModal} from '../../widgets/modal';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';
import {BigtraceAsyncDataSource} from '../query/bigtrace_async_data_source';
import {formatPerfettoSql} from '../query/sql_formatter';
import {BigtraceQueryClient} from '../query/bigtrace_query_client';
import type {QueryRunner} from '../query/query_runner';
import {
  addSubQuery,
  type BigTraceEditorTab,
  type QueryTabsState,
  applyModeDefaults,
  canRenameSubQuery,
  effectiveTabSettings,
  getSubQueryTableName,
  hasQueryText,
  removeSubQuery,
  renameSubQuery,
  selectSubQuery,
  syncActiveSubQueryToSlot,
} from './query_tabs_state';
import {formatCompact} from '../query/query_store';
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
    // configured tab shows it too while its Settings are open.
    if (
      tab.queryEngine !== 'duckdb' &&
      (!tab.configured || tab.settingsSession !== undefined)
    ) {
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
      tab.queryEngine !== 'duckdb' &&
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
      tabsState.saveNow();
    },
    getMaterialize: () => tab.materialize,
    setMaterialize: (materialize) => {
      // Same path as the toolbar switch: caps the user never touched follow
      // the mode.
      applyModeDefaults(tab, materialize);
      tabsState.saveNow();
    },
    getExperimentFilter: () => tab.experimentFilter,
    setExperimentFilter: (filter) => {
      tab.experimentFilter = filter;
      tabsState.markDirty();
    },
    getTraceLimit: () => tab.traceLimit,
    setTraceLimit: (limit) => {
      if (limit >= 0) {
        tab.traceLimit = limit;
        tabsState.saveNow();
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

// Checks if running `query` on `tab` would overwrite an existing result table
// (or this tab's previous run) and shows a Perfetto modal offering to either
// overwrite in place or create and run in a new tab with `<name>_1` (editable).
async function runTabWithOverwriteCheck(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  runner: QueryRunner,
  query: string,
): Promise<void> {
  const isMulti = tab.queryEngine !== 'duckdb' && tab.subQueries.length > 1;
  const activeTables: Array<{name: string; slotIndex?: number}> = isMulti
    ? tab.subQueries
        .map((q, i) => ({
          hasSql: hasQueryText(q.sql),
          name: getSubQueryTableName(q, i),
          slotIndex: i,
        }))
        .filter((x) => x.hasSql)
    : [{name: tab.tableName.trim() || tab.title.trim()}];

  const endpoint = getBigtraceEndpoint().trim();
  let collidingTable = '';
  let suggested = '';
  let collidingSlotIndex: number | undefined;

  if (endpoint !== '') {
    const client = new BigtraceQueryClient(endpoint);
    for (const entry of activeTables) {
      const name = entry.name.replace(/[^A-Za-z0-9_]/g, '_');
      if (!name) continue;
      try {
        const check = await client.checkTableExists(name);
        if (check.exists) {
          collidingTable = check.resolvedTableName || name;
          suggested = check.suggestedTableName ?? `${collidingTable}_1`;
          collidingSlotIndex = entry.slotIndex;
          break;
        }
      } catch {
        // Fall back to runner if check fails
      }
    }
  }

  if (!collidingTable) {
    await runner.run(tab, query);
    return;
  }

  let newTableName = suggested;
  let choice = 'cancel' as 'cancel' | 'overwrite' | 'new_tab' | string;

  await showModal({
    title: `Table "${collidingTable}" already exists`,
    content: () =>
      m(
        Stack,
        {orientation: 'vertical', gap: 'normal'},
        m(
          'div',
          'Running in this tab will overwrite it, or you can create a new tab with the suggested table name below:',
        ),
        m(TextInput, {
          value: newTableName,
          placeholder: suggested,
          autofocus: true,
          onInput: (val: string) => {
            newTableName = val;
          },
        }),
      ),
    buttons: [
      {text: 'Cancel', action: () => (choice = 'cancel')},
      {text: 'Overwrite', action: () => (choice = 'overwrite')},
      {
        text: 'Create New Tab',
        primary: true,
        action: () => (choice = 'new_tab'),
      },
    ],
  });

  if (choice === 'overwrite') {
    await runner.run(tab, query);
  } else if (choice === 'new_tab') {
    const chosen = newTableName.trim() || suggested;
    const newTab = tabsState.cloneTab(
      tab.id,
      chosen,
      query,
      collidingSlotIndex,
    );
    m.redraw();
    if (newTab) {
      await runner.run(newTab, query);
    }
  }
}

// ---------------------------------------------------------------------------
// Editor panel: toolbar (Run/Cancel + limit)
// ---------------------------------------------------------------------------

function renderEditorPanel(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  runner: QueryRunner,
  useBigtraceBackend: boolean,
): m.Children {
  const isDuckdb = tab.queryEngine === 'duckdb';
  const isMultiQuery = !isDuckdb && tab.subQueries.length > 1;
  const canRun = isMultiQuery
    ? tab.subQueries.some((q, i) =>
        hasQueryText(i === tab.activeSubQueryIndex ? tab.editorText : q.sql),
      )
    : hasQueryText(tab.editorText);

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
              label: isMultiQuery
                ? `Run ${tab.subQueries.length} Queries`
                : 'Run',
              icon: 'play_arrow',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
              disabled: !canRun,
              onclick: () => {
                if (tab.subQueries.length > 0) {
                  syncActiveSubQueryToSlot(tab);
                }
                void runTabWithOverwriteCheck(
                  tab,
                  tabsState,
                  runner,
                  tab.editorText,
                );
              },
            }),
        m(
          Stack,
          {orientation: 'horizontal', className: 'pf-bt-query-page__hotkeys'},
          'or press',
          m(HotkeyGlyphs, {hotkey: 'Mod+Enter'}),
        ),
        m('span.pf-bt-divider', {'aria-hidden': 'true'}),
        m(Button, {
          icon: 'format_align_left',
          title: 'Format query (Alt+Shift+F)',
          disabled: !hasQueryText(tab.editorText),
          onclick: () => void formatTabQuery(tab, tabsState, tab.editorText),
        }),
        isDuckdb && m(Chip, {label: 'DuckDB Mode', intent: Intent.Warning}),
        m(StackAuto),
        !isDuckdb && useBigtraceBackend && renderRunControls(tab, tabsState),
      ]),
    ]),
    isMultiQuery && renderSubQueryBar(tab, tabsState),
    !isDuckdb &&
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
      onFormat: (text: string) => formatTabQuery(tab, tabsState, text),
      onUpdate: (text: string) => {
        tab.editorText = text;
        if (
          tab.subQueries.length > 0 &&
          tab.subQueries[tab.activeSubQueryIndex] !== undefined
        ) {
          tab.subQueries[tab.activeSubQueryIndex].sql = text;
        }
        tabsState.markDirty();
      },
      onExecute: (query: string) => {
        if (tab.subQueries.length > 0) {
          syncActiveSubQueryToSlot(tab);
        }
        void runTabWithOverwriteCheck(tab, tabsState, runner, query);
      },
    }),
  ]);
}

let editingSubQueryKey: string | undefined;
let editingSubQueryValue = '';

function commitSubQueryRename(
  tab: BigTraceEditorTab,
  idx: number,
  tabsState: QueryTabsState,
): void {
  if (editingSubQueryKey === undefined) return;
  editingSubQueryKey = undefined;
  renameSubQuery(tab, idx, editingSubQueryValue);
  tabsState.markDirty();
}

function renderSubQueryBar(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
): m.Children {
  return m(
    '.pf-bt-subquery-bar',
    tab.subQueries.map((slot, idx) => {
      const isActive = idx === tab.activeSubQueryIndex;
      const resolvedTbl = getSubQueryTableName(slot, idx);
      const exec = slot.execution;
      const rows = exec?.processedRows ?? slot.queryResult?.totalRowCount;
      const rowsText =
        rows !== undefined && rows > 0 ? ` (${formatCompact(rows)})` : '';
      const renamable = canRenameSubQuery(tab, slot);
      const editKey = `${tab.id}:${slot.id}`;
      const isEditing = renamable && editingSubQueryKey === editKey;
      return m(
        'span.pf-bt-subquery-pill',
        {
          key: slot.id,
          className: isActive ? 'pf-bt-subquery-pill--active' : undefined,
        },
        isEditing
          ? [
              m('span.pf-bt-subquery-pill__num', `#${idx + 1}`),
              m('input.pf-bt-subquery-rename-input', {
                type: 'text',
                value: editingSubQueryValue,
                oncreate: (vnode: m.VnodeDOM) => {
                  const el = vnode.dom as HTMLInputElement;
                  el.focus();
                  el.select();
                },
                onclick: (e: Event) => e.stopPropagation(),
                oninput: (e: InputEvent) => {
                  editingSubQueryValue = (e.target as HTMLInputElement).value;
                },
                onblur: () => {
                  commitSubQueryRename(tab, idx, tabsState);
                },
                onkeydown: (e: KeyboardEvent) => {
                  if (e.key === 'Enter') {
                    e.preventDefault();
                    e.stopPropagation();
                    commitSubQueryRename(tab, idx, tabsState);
                  } else if (e.key === 'Escape') {
                    e.preventDefault();
                    e.stopPropagation();
                    editingSubQueryKey = undefined;
                  }
                },
              }),
            ]
          : m(Button, {
              compact: true,
              active: isActive,
              variant: isActive ? ButtonVariant.Filled : ButtonVariant.Minimal,
              label: `#${idx + 1} ${resolvedTbl}${rowsText}`,
              title: renamable
                ? 'Double-click to rename result table'
                : 'Result table name cannot be changed after running',
              onclick: () => {
                selectSubQuery(tab, idx);
                tabsState.markDirty();
              },
              ondblclick: renamable
                ? () => {
                    selectSubQuery(tab, idx);
                    editingSubQueryKey = editKey;
                    editingSubQueryValue = resolvedTbl;
                    tabsState.markDirty();
                  }
                : undefined,
            }),
        m(Button, {
          compact: true,
          icon: 'close',
          title: `Remove query #${idx + 1}`,
          disabled: tab.isLoading,
          onclick: () => {
            removeSubQuery(tab, idx);
            tabsState.markDirty();
          },
        }),
      );
    }),
    m(Button, {
      compact: true,
      icon: 'add',
      label: 'Add Query',
      disabled: tab.isLoading,
      onclick: () => {
        addSubQuery(tab);
        tabsState.markDirty();
      },
    }),
  );
}

// Persistent switch, row cap, and settings.
function renderRunControls(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
): m.Children {
  return [
    tab.queryEngine !== 'duckdb' &&
      tab.subQueries.length <= 1 &&
      m(Button, {
        icon: 'add',
        label: 'Add Query',
        compact: true,
        disabled: tab.isLoading,
        onclick: () => {
          addSubQuery(tab);
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
    m('span.pf-bt-divider', {'aria-hidden': 'true'}),
    m('span', 'Rows:'),
    m(TextInput, {
      type: 'number',
      className: 'pf-bt-limit-input',
      value: String(tab.limit),
      title: 'Maximum rows this query returns (0 = unlimited).',
      disabled: tab.isLoading,
      onInput: (value: string) => {
        if (value.trim() === '') {
          tab.limit = 0;
          tabsState.saveNow();
          return;
        }
        const newLimit = parseInt(value, 10);
        if (!isNaN(newLimit) && newLimit >= 0) {
          tab.limit = newLimit;
          tabsState.saveNow();
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
