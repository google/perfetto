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
import {Button, ButtonVariant} from '../../widgets/button';
import {TextInput} from '../../widgets/text_input';
import {Editor} from '../../widgets/editor';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {
  SchemaRegistry,
  ColumnSchema,
} from '../../components/widgets/datagrid/datagrid_schema';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {SplitPanel} from '../../widgets/split_panel';
import {EmptyState} from '../../widgets/empty_state';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {Box} from '../../widgets/box';
import {Stack, StackAuto} from '../../widgets/stack';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {DataSource} from '../../components/widgets/datagrid/data_source';
import {queryHistoryStorage} from '../query/query_history_storage';
import {QueryHistoryComponent} from '../query/query_history';
import {sqlTablesLoader} from '../query/sql_tables';
import {TableList} from '../../plugins/dev.perfetto.QueryPage/table_list';
import {Spinner} from '../../widgets/spinner';
import {SettingFilter} from '../settings/settings_types';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {endpointStorage} from '../settings/endpoint_storage';
import {HttpDataSource} from '../query/http_data_source';
import {Tabs, TabsTab} from '../../widgets/tabs';
import {linkify} from '../../widgets/anchor';
import {shortUuid} from '../../base/uuid';
import {Row as DataGridRow} from '../../trace_processor/query_result';
import {debounce} from '../../base/rate_limiters';

interface QueryResponse {
  query: string;
  error?: string;
  totalRowCount: number;
  durationMs: number;
  columns: string[];
  rows: DataGridRow[];
  statementCount: number;
  statementWithOutputCount: number;
  lastStatementSql: string;
}

interface QueryPageAttrs {
  useBrushBackend?: boolean;
  initialQuery?: string;
}

const DEFAULT_SQL = '';
const QUERY_TABS_STORAGE_KEY = 'bigtraceQueryTabs';

// Per-tab state for each editor tab.
interface BigTraceEditorTab {
  readonly id: string;
  title: string;
  editorText: string;
  limit: number;
  queryResult?: QueryResponse;
  isLoading: boolean;
  dataSource?: DataSource;
  querySettings: SettingFilter[];
  activeHttpDataSource?: HttpDataSource;
}

// Manages the collection of editor tabs. Survives component re-mounts.
class QueryTabsState {
  tabs: BigTraceEditorTab[] = [];
  activeTabId = '';
  private tabCounter = 0;
  private readonly debouncedSave = debounce(() => this.saveToStorage(), 1000);

  constructor() {
    if (!this.loadFromStorage()) {
      this.addNewTab(undefined, DEFAULT_SQL);
    }
  }

  private saveToStorage(): void {
    const state = {
      tabs: this.tabs.map((t) => ({
        id: t.id,
        title: t.title,
        editorText: t.editorText,
        limit: t.limit,
      })),
      activeTabId: this.activeTabId,
    };
    localStorage.setItem(QUERY_TABS_STORAGE_KEY, JSON.stringify(state));
  }

  private loadFromStorage(): boolean {
    const stored = localStorage.getItem(QUERY_TABS_STORAGE_KEY);
    if (!stored) return false;
    try {
      const parsed = JSON.parse(stored);
      if (!Array.isArray(parsed.tabs) || parsed.tabs.length === 0) return false;
      for (const t of parsed.tabs) {
        this.addNewTab(t.title, t.editorText, t.limit);
      }
      if (typeof parsed.activeTabId === 'string') {
        const found = this.tabs.find((t) => t.id === parsed.activeTabId);
        if (!found) {
          // Restored tabs get new IDs, so activate by index instead.
          const idx = parsed.tabs.findIndex(
            (t: {id: string}) => t.id === parsed.activeTabId,
          );
          if (idx >= 0 && idx < this.tabs.length) {
            this.activeTabId = this.tabs[idx].id;
          }
        }
      }
      return true;
    } catch {
      return false;
    }
  }

  markDirty(): void {
    this.debouncedSave();
  }

  private nextTabName(): string {
    const existingNames = new Set(this.tabs.map((t) => t.title));
    let count = ++this.tabCounter;
    while (existingNames.has(`Query ${count}`)) {
      count = ++this.tabCounter;
    }
    return `Query ${count}`;
  }

  addNewTab(
    title?: string,
    initialQuery?: string,
    limit?: number,
  ): BigTraceEditorTab {
    const tab: BigTraceEditorTab = {
      id: shortUuid(),
      title: title ?? this.nextTabName(),
      editorText: initialQuery ?? '',
      limit: limit ?? 100,
      queryResult: undefined,
      isLoading: false,
      dataSource: undefined,
      querySettings: [],
      activeHttpDataSource: undefined,
    };
    this.tabs.push(tab);
    this.activeTabId = tab.id;
    this.markDirty();
    return tab;
  }

  getActiveTab(): BigTraceEditorTab | undefined {
    return this.tabs.find((t) => t.id === this.activeTabId);
  }

  closeTab(tabId: string): void {
    if (this.tabs.length <= 1) return;
    const index = this.tabs.findIndex((t) => t.id === tabId);
    if (index === -1) return;
    this.tabs[index].activeHttpDataSource?.abort();
    this.tabs.splice(index, 1);
    if (this.activeTabId === tabId) {
      const newIndex = Math.min(index, this.tabs.length - 1);
      this.activeTabId = this.tabs[newIndex].id;
    }
    this.markDirty();
  }

  renameTab(tabId: string, newTitle: string): void {
    const tab = this.tabs.find((t) => t.id === tabId);
    if (tab) {
      tab.title = newTitle;
      this.markDirty();
    }
  }

  reorderTab(draggedId: string, beforeId: string | undefined): void {
    const draggedIndex = this.tabs.findIndex((t) => t.id === draggedId);
    if (draggedIndex === -1) return;
    const [dragged] = this.tabs.splice(draggedIndex, 1);
    if (beforeId === undefined) {
      this.tabs.push(dragged);
    } else {
      const beforeIndex = this.tabs.findIndex((t) => t.id === beforeId);
      if (beforeIndex === -1) {
        this.tabs.push(dragged);
      } else {
        this.tabs.splice(beforeIndex, 0, dragged);
      }
    }
  }
}

const tabsState = new QueryTabsState();

export class QueryPage implements m.ClassComponent<QueryPageAttrs> {
  private useBrushBackend = false;
  private sidebarVisible = true;

  oninit({attrs}: m.Vnode<QueryPageAttrs>) {
    this.useBrushBackend = attrs.useBrushBackend || false;
    if (attrs.initialQuery) {
      const activeTab = tabsState.getActiveTab();
      if (activeTab && activeTab.editorText.trim() === '') {
        activeTab.editorText = attrs.initialQuery;
      } else {
        tabsState.addNewTab(undefined, attrs.initialQuery);
      }
      tabsState.markDirty();
    }
    if (this.useBrushBackend) {
      bigTraceSettingsStorage.loadSettings();
    }
    sqlTablesLoader.load();
  }

  view() {
    const activeTab = tabsState.getActiveTab();

    // Build editor tabs for the Tabs widget.
    const editorTabs: TabsTab[] = tabsState.tabs.map((tab) => ({
      key: tab.id,
      title: tab.title,
      leftIcon: 'code',
      closeButton: tabsState.tabs.length > 1,
      content: this.renderEditorTabContent(tab),
    }));

    const leftPanel = m(Tabs, {
      className: 'pf-query-page__editor-tabs',
      tabs: editorTabs,
      activeTabKey: tabsState.activeTabId,
      reorderable: true,
      onTabChange: (key) => {
        tabsState.activeTabId = key;
        tabsState.markDirty();
      },
      onTabRename: (key, newTitle) => tabsState.renameTab(key, newTitle),
      onTabClose: (key) => tabsState.closeTab(key),
      onTabReorder: (draggedKey, beforeKey) =>
        tabsState.reorderTab(draggedKey, beforeKey),
      newTabContent: [
        m(Button, {
          icon: 'add',
          className: 'pf-tabs__new-tab-btn',
          onclick: () => tabsState.addNewTab(),
        }),
        m('div', {style: {flex: '1'}}),
        m(Button, {
          icon: this.sidebarVisible ? 'right_panel_close' : 'right_panel_open',
          title: this.sidebarVisible ? 'Hide sidebar' : 'Show sidebar',
          onclick: () => {
            this.sidebarVisible = !this.sidebarVisible;
          },
          active: this.sidebarVisible,
        }),
      ],
    });

    const sidebarPanel = m(Tabs, {
      className: 'pf-query-page__sidebar',
      tabs: [
        {
          key: 'history',
          title: 'History',
          leftIcon: 'history',
          content: m(QueryHistoryComponent, {
            className: 'pf-query-page__history',
            runQuery: (query: string) => {
              if (activeTab) this.runQueryOnTab(activeTab, query);
            },
            setQuery: (query: string) => {
              if (activeTab) activeTab.editorText = query;
            },
          }),
        },
        {
          key: 'tables',
          title: 'Tables',
          leftIcon: 'table_chart',
          content: this.renderTablesTab(),
        },
      ],
    });

    if (!this.sidebarVisible) {
      return m('.pf-query-page', leftPanel);
    }

    return m(
      '.pf-query-page',
      m(SplitPanel, {
        direction: 'horizontal',
        initialSplit: {percent: 25},
        controlledPanel: 'second',
        minSize: 100,
        firstPanel: leftPanel,
        secondPanel: sidebarPanel,
      }),
    );
  }

  private renderEditorTabContent(tab: BigTraceEditorTab): m.Children {
    const editorPanel = m('.pf-query-page__editor-panel', [
      m(Box, {className: 'pf-query-page__toolbar'}, [
        m(Stack, {orientation: 'horizontal'}, [
          tab.isLoading
            ? m(Button, {
                label: 'Cancel',
                icon: 'stop',
                intent: Intent.Warning,
                variant: ButtonVariant.Filled,
                onclick: () => this.cancelQueryOnTab(tab),
              })
            : m(Button, {
                label: 'Run Query',
                icon: 'play_arrow',
                intent: Intent.Primary,
                variant: ButtonVariant.Filled,
                onclick: () => this.runQueryOnTab(tab, tab.editorText),
              }),
          m(
            Stack,
            {
              orientation: 'horizontal',
              className: 'pf-query-page__hotkeys',
            },
            'or press',
            m(HotkeyGlyphs, {hotkey: 'Mod+Enter'}),
          ),
          m(StackAuto),
          this.useBrushBackend && [
            m('span', 'Result limit:'),
            m(TextInput, {
              type: 'number',
              value: String(tab.limit),
              placeholder: 'Limit',
              onChange: (value: string) => {
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
        onUpdate: (text: string) => {
          tab.editorText = text;
          tabsState.markDirty();
        },
        onExecute: (query: string) => this.runQueryOnTab(tab, query),
      }),
    ]);

    const resultsPanel = m(
      '.pf-query-page__results-panel',
      tab.dataSource && tab.queryResult
        ? this.renderQueryResult(
            tab.queryResult,
            tab.dataSource,
            tab.querySettings,
          )
        : tab.isLoading
          ? m(EmptyState, {
              title: 'Running query...',
              icon: 'hourglass_empty',
              fillHeight: true,
            })
          : m(EmptyState, {
              title: 'Run a query to see results',
              icon: 'search',
              fillHeight: true,
            }),
    );

    return m(SplitPanel, {
      direction: 'vertical',
      initialSplit: {percent: 35},
      minSize: 100,
      firstPanel: editorPanel,
      secondPanel: resultsPanel,
    });
  }

  private renderTablesTab(): m.Children {
    if (sqlTablesLoader.loadError) {
      return m(EmptyState, {
        title: `Failed to load tables: ${sqlTablesLoader.loadError}`,
        icon: 'error',
        fillHeight: true,
      });
    }
    const modules = sqlTablesLoader.modules;
    if (sqlTablesLoader.isLoading || !modules) {
      return m(
        EmptyState,
        {
          title: 'Loading tables...',
          icon: 'hourglass_empty',
          fillHeight: true,
        },
        m(Spinner),
      );
    }
    return m(TableList, {
      sqlModules: modules,
      onQueryTable: (tableName, query) => {
        tabsState.addNewTab(tableName, query);
      },
    });
  }

  private cancelQueryOnTab(tab: BigTraceEditorTab) {
    tab.activeHttpDataSource?.abort();
    tab.activeHttpDataSource = undefined;
    tab.isLoading = false;
    m.redraw();
  }

  private async runQueryOnTab(tab: BigTraceEditorTab, query: string) {
    if (!query) return;

    // Abort any in-flight query on this tab.
    tab.activeHttpDataSource?.abort();

    queryHistoryStorage.saveQuery(query);

    tab.isLoading = true;
    tab.queryResult = undefined;
    m.redraw();

    if (this.useBrushBackend) {
      const endpointSetting = endpointStorage.get('bigtraceEndpoint');
      const endpoint = endpointSetting ? (endpointSetting.get() as string) : '';

      await bigTraceSettingsStorage.loadSettings();

      const settings = bigTraceSettingsStorage.buildSettingFilters();
      tab.querySettings = settings;

      const httpDataSource = new HttpDataSource(
        endpoint,
        query,
        tab.limit,
        settings,
      );
      tab.activeHttpDataSource = httpDataSource;
      const startMs = performance.now();
      try {
        const data = await httpDataSource.query();
        tab.queryResult = {
          rows: data,
          columns: data.length > 0 ? Object.keys(data[0]) : [],
          error: undefined,
          totalRowCount: data.length,
          durationMs: performance.now() - startMs,
          statementWithOutputCount: 1,
          statementCount: 1,
          lastStatementSql: query,
          query,
        };
      } catch (e) {
        // Don't show an error for user-initiated cancellation.
        if (e instanceof Error && e.message === 'Query was cancelled.') {
          return;
        }
        const error = e instanceof Error ? e.message : String(e);
        tab.queryResult = {
          rows: [],
          columns: [],
          error,
          totalRowCount: 0,
          durationMs: performance.now() - startMs,
          statementWithOutputCount: 0,
          statementCount: 1,
          lastStatementSql: query,
          query,
        };
      } finally {
        tab.activeHttpDataSource = undefined;
      }
    } else {
      throw new Error(
        'Local query execution is unsupported in bigtrace context.',
      );
    }

    if (tab.queryResult !== undefined) {
      tab.dataSource = new InMemoryDataSource(tab.queryResult.rows);
    }

    tab.isLoading = false;
    m.redraw();
  }

  private renderQueryResult(
    queryResult: QueryResponse,
    dataSource: DataSource,
    querySettings: SettingFilter[],
  ) {
    if (queryResult.error) {
      return m(
        '.pf-query-page__query-error',
        `Error (after ${Math.round(queryResult.durationMs).toLocaleString()} ms): ${queryResult.error}`,
      );
    } else {
      const tableContent = [
        queryResult.statementWithOutputCount > 1 &&
          m(Box, [
            m(Callout, {icon: 'warning', intent: Intent.None}, [
              `${queryResult.statementWithOutputCount} out of ${queryResult.statementCount} `,
              'statements returned a result. ',
              'Only the results for the last statement are displayed.',
            ]),
          ]),
        (() => {
          // Build schema directly
          const columnSchema: ColumnSchema = {};
          for (const column of queryResult.columns) {
            if (column === 'link') {
              columnSchema[column] = {
                cellRenderer: (value) => {
                  if (value === null || value === undefined) {
                    return '';
                  }
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
            enablePivotControls: false, // In-memory datasource does not support pivoting
            initialColumns: queryResult.columns
              .filter((col) => {
                if (!col.startsWith('_')) return true;
                if (col === '_trace_id') return true;
                const settingId = col.substring(1);
                return querySettings.some(
                  (s) =>
                    s.settingId === settingId &&
                    s.category === 'TRACE_METADATA',
                );
              })
              .map((col) => ({
                id: col,
                field: col,
              })),
            className: 'pf-query-page__results',
            data: dataSource,
            showExportButton: true,
            emptyStateMessage: 'Query returned no rows',
            toolbarItemsLeft: m(
              'span.pf-query-page__results-summary',
              `Returned ${queryResult.totalRowCount.toLocaleString()} rows in ${Math.round(queryResult.durationMs).toLocaleString()} ms`,
            ),
            toolbarItemsRight: [
              m(CopyToClipboardButton, {
                textToCopy: queryResult.query,
                title: 'Copy executed query to clipboard',
                label: 'Copy Query',
              }),
            ],
          });
        })(),
      ];

      return m(Tabs, {
        tabs: [
          {
            key: 'table',
            title: 'Table',
            content: tableContent,
          },
          {
            key: 'chart',
            title: 'Chart',
            content: m(EmptyState, {
              title: 'Charts are coming soon',
              icon: 'bar_chart',
            }),
          },
        ],
      });
    }
  }
}
