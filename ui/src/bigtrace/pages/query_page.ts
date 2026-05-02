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
import {Select} from '../../widgets/select';
import {Box} from '../../widgets/box';
import {Stack, StackAuto} from '../../widgets/stack';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {DataSource} from '../../components/widgets/datagrid/data_source';
import {QueryHistoryComponent} from '../query/query_history';
import {Switch} from '../../widgets/switch';
import {sqlTablesLoader} from '../query/sql_tables';
import {TableList} from '../query/table_list';
import {Spinner} from '../../widgets/spinner';
import {Duration} from '../../base/time';
import {Icon} from '../../widgets/icon';
import {BigtraceAsyncDataSource} from '../query/bigtrace_async_data_source';
import {LinearProgress} from '../../widgets/linear_progress';
import {SettingFilter} from '../settings/settings_types';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {endpointStorage} from '../settings/endpoint_storage';
import {HttpDataSource} from '../query/http_data_source';
import {Tabs, TabsTab} from '../../widgets/tabs';
import {linkify} from '../../widgets/anchor';
import {shortUuid} from '../../base/uuid';
import {QueryExecution, queryStore} from '../query/query_store';
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
  useBigtraceBackend?: boolean;
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
  materialize: boolean;
  queryUuid?: string;
  pollInterval?: number;
  currentOffset: number;
  lastProcessedRows: number;
  pageSize: number;
  clientStartTime?: number;
  execution?: QueryExecution;
}

interface StoredTab {
  id: string;
  title: string;
  editorText: string;
  limit: number;
  materialize: boolean;
  queryUuid?: string;
  error?: string;
}

// Manages the collection of editor tabs. Survives component re-mounts.
class QueryTabsState {
  tabs: BigTraceEditorTab[] = [];
  activeTabId = '';
  private tabCounter = 0;
  globalPageSize = 50;
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
        materialize: t.materialize,
        queryUuid: t.queryUuid,
        error: t.queryResult?.error,
      })),
      activeTabId: this.activeTabId,
      globalPageSize: this.globalPageSize,
    };
    localStorage.setItem(QUERY_TABS_STORAGE_KEY, JSON.stringify(state));
  }

  private loadFromStorage(): boolean {
    const stored = localStorage.getItem(QUERY_TABS_STORAGE_KEY);
    if (!stored) return false;
    try {
      const parsed = JSON.parse(stored) as {
        tabs: StoredTab[];
        activeTabId?: string;
        globalPageSize?: number;
      };
      if (!Array.isArray(parsed.tabs) || parsed.tabs.length === 0) return false;
      if (parsed.globalPageSize !== undefined) {
        this.globalPageSize = parsed.globalPageSize;
      }
      for (const t of parsed.tabs) {
        const tab = this.addNewTab(
          t.title,
          t.editorText,
          t.limit,
          t.queryUuid,
          t.materialize,
        );
        if (t.error !== undefined && t.error !== '') {
          tab.queryResult = {
            rows: [],
            columns: [],
            error: t.error,
            totalRowCount: 0,
            durationMs: 0,
            statementWithOutputCount: 0,
            statementCount: 1,
            lastStatementSql: tab.editorText,
            query: tab.editorText,
          };
        }
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
    queryUuid?: string,
    materialize?: boolean,
    forceNew?: boolean,
  ): BigTraceEditorTab {
    if (!forceNew) {
      const existingTab = this.tabs.find((t) => {
        if (queryUuid && t.queryUuid === queryUuid) return true;
        if (!queryUuid && initialQuery && t.editorText === initialQuery) {
          return true;
        }
        return false;
      });

      if (existingTab) {
        this.activeTabId = existingTab.id;
        this.markDirty();
        return existingTab;
      }
    }

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
      materialize: materialize ?? (queryUuid ? true : false),
      currentOffset: 0,
      lastProcessedRows: 0,
      pageSize: this.globalPageSize,
      queryUuid,
    };
    tab.execution = queryStore.getOrCreate(queryUuid || tab.id, {
      materialized: tab.materialize,
    });
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
    const tabToClose = this.tabs[index];
    if (tabToClose.pollInterval !== undefined) {
      console.log('closeTab: clearing poll interval', tabToClose.pollInterval);
      window.clearTimeout(tabToClose.pollInterval);
      tabToClose.pollInterval = undefined;
    }
    tabToClose.activeHttpDataSource?.abort();
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
  private useBigtraceBackend = false;
  private sidebarVisible = true;
  private historyRefreshSignal = 0;

  oninit({attrs}: m.Vnode<QueryPageAttrs>) {
    this.useBigtraceBackend = attrs.useBigtraceBackend || false;
    if (attrs.initialQuery) {
      const activeTab = tabsState.getActiveTab();
      if (activeTab && activeTab.editorText.trim() === '') {
        activeTab.editorText = attrs.initialQuery;
      } else {
        tabsState.addNewTab(undefined, attrs.initialQuery);
      }
      tabsState.markDirty();
    }
    if (this.useBigtraceBackend) {
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
            refreshSignal: this.historyRefreshSignal,
            setQuery: (query: string) => {
              if (activeTab) activeTab.editorText = query;
            },
            openQuery: async (
              query: string,
              uuid: string,
              materialize: boolean,
              forceNew?: boolean,
              limit?: number,
              startTime?: string,
            ) => {
              const tab = tabsState.addNewTab(
                undefined,
                query,
                limit,
                uuid,
                materialize,
                forceNew,
              );

              // Ensure the potentially existing tab is made active
              tabsState.activeTabId = tab.id;
              tabsState.markDirty();

              if (startTime && tab.execution) {
                tab.execution.startTime = new Date(startTime).getTime();
              }

              const endpointSetting = endpointStorage.get('bigtraceEndpoint');
              const endpoint = endpointSetting
                ? (endpointSetting.get() as string)
                : '';
              const httpDataSource = new HttpDataSource(endpoint, '', 50, []);
              tab.activeHttpDataSource = httpDataSource;

              // Create DataSource immediately to avoid race condition with startPolling!
              // Initialize DataSource only if it doesn't exist or is of the wrong type
              if (
                !tab.dataSource ||
                (materialize &&
                  !(tab.dataSource instanceof BigtraceAsyncDataSource))
              ) {
                if (materialize) {
                  tab.dataSource = new BigtraceAsyncDataSource(
                    uuid,
                    httpDataSource,
                    () =>
                      tab.execution?.processedRows !== undefined
                        ? tab.execution.processedRows
                        : 0,
                    () => tab.currentOffset,
                    () => tab.pageSize,
                  );
                } else {
                  tab.dataSource = new InMemoryDataSource([]);
                }
              }

              try {
                // Fetch full details FIRST to get full SQL and status!
                const details = await httpDataSource.getQueryExecution(uuid);
                if (details !== undefined && details !== null) {
                  // Use perfettoSql or perfetto_sql as returned by backend!
                  tab.editorText = details.perfettoSql || query;

                  // Update limit if present!
                  if (details.limit !== undefined) {
                    tab.limit = Number(details.limit);
                  }

                  if (tab.execution !== undefined) {
                    tab.execution.status = details.status || 'N/A';
                    tab.execution.processedRows =
                      details.processedRows !== undefined
                        ? details.processedRows
                        : 0;
                    tab.execution.processedTraces =
                      details.processedTraces !== undefined
                        ? details.processedTraces
                        : 0;
                    tab.execution.totalTraces =
                      details.totalTraces !== undefined
                        ? details.totalTraces
                        : 0;

                    const startTime = details.startTime;
                    if (startTime !== undefined) {
                      tab.execution.startTime = new Date(startTime).getTime();
                    }

                    const isTerminal =
                      tab.execution.status === 'SUCCESS' ||
                      tab.execution.status === 'FAILED' ||
                      tab.execution.status === 'CANCELLED';

                    const endTime = details.endTime;
                    if (
                      isTerminal &&
                      startTime !== undefined &&
                      endTime !== undefined
                    ) {
                      const end = new Date(endTime).getTime();
                      tab.execution.endTime = end;
                    }

                    tab.isLoading = !isTerminal;

                    // Initialize queryResult if it doesn't exist, for any state.
                    if (!tab.queryResult) {
                      tab.queryResult = {
                        rows: [], // Managed by DataSource
                        columns: [], // Managed by DataSource
                        error: undefined,
                        totalRowCount: tab.execution.processedRows,
                        durationMs: 0, // Will be updated for terminal states
                        statementWithOutputCount: 1,
                        statementCount: 1,
                        lastStatementSql: tab.editorText,
                        query: tab.editorText,
                      };
                    } else {
                      // Update fields that might change on re-opening
                      tab.queryResult.totalRowCount =
                        tab.execution.processedRows;
                      tab.queryResult.lastStatementSql = tab.editorText;
                      tab.queryResult.query = tab.editorText;
                    }

                    if (!isTerminal) {
                      this.startPolling(tab);
                    } else {
                      // Terminal state: Ensure results are loaded if necessary

                      const durationMs =
                        tab.execution.endTime !== undefined &&
                        tab.execution.startTime !== undefined
                          ? tab.execution.endTime - tab.execution.startTime
                          : 0;

                      if (
                        tab.execution.status === 'SUCCESS' ||
                        tab.execution.status === 'CANCELLED'
                      ) {
                        if (tab.dataSource instanceof BigtraceAsyncDataSource) {
                          // *** CRITICAL: Call method to load results ***
                          await tab.dataSource.ensureResultsLoaded(tab);

                          // Also need to ensure queryResult is initialized so UI renders results container!
                          if (tab.queryResult === undefined) {
                            tab.queryResult = {
                              rows: [],
                              columns: [],
                              error: undefined,
                              totalRowCount: tab.execution.processedRows,
                              durationMs,
                              statementWithOutputCount: 1,
                              statementCount: 1,
                              lastStatementSql: tab.editorText,
                              query: tab.editorText,
                            };
                          }
                        }
                      } else if (tab.execution.status === 'FAILED') {
                        const error =
                          details.errorMessage ||
                          'Query failed without a specific error message.';
                        tab.queryResult = {
                          rows: [],
                          columns: [],
                          error,
                          totalRowCount: 0,
                          durationMs,
                          statementWithOutputCount: 1,
                          statementCount: 1,
                          lastStatementSql: tab.editorText,
                          query: tab.editorText,
                        };
                      }
                    }
                  }
                  m.redraw();
                }
              } catch (e) {
                console.error('Failed to fetch query details on open:', e);
                // Fallback to polling if details fetch fails!
                this.startPolling(tab);
              }
            },
          }),
        },
        {
          key: 'tables',
          title: 'Stdlib Schemas',
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
    if (tab.queryUuid && !tab.dataSource) {
      const endpointSetting = endpointStorage.get('bigtraceEndpoint');
      const endpoint = endpointSetting ? (endpointSetting.get() as string) : '';
      const httpDataSource = new HttpDataSource(
        endpoint,
        tab.editorText,
        tab.limit,
        tab.querySettings,
      );
      tab.activeHttpDataSource = httpDataSource;
      tab.dataSource = new BigtraceAsyncDataSource(
        tab.queryUuid,
        httpDataSource,
        () =>
          tab.execution?.processedRows !== undefined
            ? tab.execution.processedRows
            : 0,
        () => tab.currentOffset,
        () => tab.pageSize,
      );
      tab.isLoading = true;
      this.startPolling(tab);

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
          this.useBigtraceBackend && [
            tab.isLoading && m(LinearProgress, {state: 'indeterminate'}),
            m('span', 'Result limit:'),
            m(TextInput, {
              type: 'number',
              value: String(tab.limit),
              placeholder: 'Limit',
              onInput: (value: string) => {
                const newLimit = parseInt(value, 10);
                if (!isNaN(newLimit) && newLimit > 0) {
                  tab.limit = newLimit;
                }
              },
            }),
            m(Switch, {
              label: 'Materialize?',
              checked: tab.materialize,
              onchange: (e: Event) => {
                tab.materialize = (e.target as HTMLInputElement).checked;
                tabsState.markDirty();
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

    if (tab.dataSource && tab.queryResult && tab.materialize && tab.execution) {
      tab.queryResult.totalRowCount = tab.execution.processedRows;
    }

    const isTerminal =
      tab.execution?.status === 'SUCCESS' ||
      tab.execution?.status === 'FAILED' ||
      tab.execution?.status === 'CANCELLED';

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

    const statusBox =
      tab.materialize &&
      tab.queryUuid &&
      m(
        Box,
        {
          style: {
            padding: '10px',
            borderBottom: '1px solid var(--p-color-border)',
          },
        },
        [
          m(
            Stack,
            {orientation: 'horizontal', gap: '10px', alignItems: 'center'},
            [
              m(
                'div',
                {style: {position: 'relative', display: 'inline-block'}},
                [
                  m(Button, {
                    icon: 'refresh',
                    title:
                      (tab.execution?.processedRows !== undefined
                        ? tab.execution.processedRows
                        : 0) > tab.lastProcessedRows
                        ? 'New data available. Click to refresh.'
                        : 'Refresh data',
                    onclick: async () => {
                      console.log('Refresh Data button clicked');
                      if (tab.queryUuid !== undefined && tab.queryUuid !== '') {
                        try {
                          const status =
                            await tab.activeHttpDataSource?.getStatus(
                              tab.queryUuid,
                            );
                          if (status !== undefined && status !== null) {
                            queryStore.update(tab.queryUuid, {
                              processedRows:
                                status.processedRows !== undefined
                                  ? status.processedRows
                                  : 0,
                              processedTraces:
                                status.processedTraces !== undefined
                                  ? status.processedTraces
                                  : 0,
                              totalTraces:
                                status.totalTraces !== undefined
                                  ? status.totalTraces
                                  : 0,
                              status: status.status || 'N/A',
                            });
                          }
                        } catch (error) {
                          console.error(
                            'Failed to fetch query status on refresh:',
                            error,
                          );
                        }
                      }
                      if (tab.dataSource instanceof BigtraceAsyncDataSource) {
                        tab.dataSource.triggerFetch(
                          tab.currentOffset,
                          tab.pageSize,
                        );
                        tab.lastProcessedRows =
                          tab.execution?.processedRows !== undefined
                            ? tab.execution.processedRows
                            : 0;
                      }
                      m.redraw();
                    },
                  }),
                  !isTerminal &&
                    (tab.execution?.processedRows !== undefined
                      ? tab.execution.processedRows
                      : 0) > tab.lastProcessedRows &&
                    m('span', {
                      style: {
                        position: 'absolute',
                        top: '-2px',
                        right: '-2px',
                        width: '8px',
                        height: '8px',
                        borderRadius: '50%',
                        backgroundColor: 'var(--pf-green-color, #0f9d58)',
                        zIndex: 10,
                      },
                    }),
                ],
              ),
              m(
                'span',
                `Status: ${tab.execution?.status !== undefined ? tab.execution.status : 'N/A'} | Traces: ${tab.execution?.processedTraces !== undefined ? tab.execution.processedTraces : 0}/${tab.execution?.totalTraces !== undefined ? tab.execution.totalTraces : 0} | Rows: ${tab.execution?.processedRows !== undefined ? tab.execution.processedRows : 0} | Duration: ${Duration.humanise(Duration.fromMillis(durationMs))}`,
              ),
            ],
          ),
        ],
      );

    const resultsPanel = m(
      '.pf-query-page__results-panel',
      statusBox,
      tab.dataSource && tab.queryResult
        ? m('.pf-query-page__results-container', [
            tab.queryResult.error &&
              (() => {
                const errorStr = tab.queryResult.error;
                let displayTitle = 'Query failed';
                let displayMessage = '';

                if (
                  errorStr.includes('FAILED_PRECONDITION') ||
                  errorStr.includes('failed_precondition')
                ) {
                  displayTitle = 'Results no longer available';
                  displayMessage =
                    'The materialized table for this query has expired. You may need to run the query again.';
                }

                return m(
                  '.pf-results-table__error',
                  m(Icon, {
                    className: 'pf-results-table__error-icon',
                    icon: 'error',
                    intent: Intent.Danger,
                  }),
                  m(
                    'details',
                    {
                      open:
                        (tab.execution?.processedRows !== undefined
                          ? tab.execution.processedRows
                          : 0) === 0,
                    },
                    [
                      m(
                        'summary',
                        {style: {cursor: 'pointer', fontWeight: 'bold'}},
                        displayTitle,
                      ),
                      displayMessage &&
                        m(
                          'div',
                          {style: {marginTop: '5px', marginBottom: '10px'}},
                          displayMessage,
                        ),
                      m(
                        'pre.pf-results-table__error-message',
                        {
                          style: {
                            overflow: 'auto',
                            maxWidth: '100%',
                            textAlign: 'left',
                            marginTop: '10px',
                            fontSize: '0.85em',
                            opacity: 0.8,
                          },
                        },
                        errorStr
                          .replaceAll('\\n', '\n')
                          .replaceAll('\\t', '  ')
                          .replaceAll('\\u003e', '>'),
                      ),
                    ],
                  ),
                );
              })(),
            (tab.execution?.processedRows !== undefined
              ? tab.execution.processedRows
              : 0) > 0
              ? this.renderQueryResult(
                  tab.queryResult,
                  tab.dataSource,
                  tab.querySettings,
                  tab,
                )
              : tab.isLoading
                ? m('div')
                : !tab.queryResult.error &&
                  m(EmptyState, {
                    title: 'Query returned no rows',
                    icon: 'search',
                    fillHeight: true,
                  }),
          ])
        : tab.isLoading
          ? m('div')
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

  private async cancelQueryOnTab(tab: BigTraceEditorTab) {
    m.redraw(); // Update UI to show cancelling state

    const dataSource = tab.activeHttpDataSource;
    const queryUuid = tab.queryUuid;

    // Abort any ongoing client-side poll/request regardless
    dataSource?.abort();
    if (tab.pollInterval !== undefined) {
      window.clearTimeout(tab.pollInterval);
      tab.pollInterval = undefined;
    }

    if (tab.materialize && queryUuid && dataSource) {
      console.log(`Attempting to cancel query: ${queryUuid}`);
      try {
        await dataSource.cancelQuery(queryUuid);
        console.log(
          `cancelQueryOnTab: query ${queryUuid} cancelled on backend, refreshing history`,
        );
        this.historyRefreshSignal++;
      } catch (e) {
        console.error(`Failed to cancel query ${queryUuid} on backend:`, e);
      } finally {
        tab.activeHttpDataSource = undefined;
        tab.isLoading = false;
        m.redraw();
      }
    } else {
      console.warn('cancelQueryOnTab: Cancellation not sent to backend.', {
        materialize: tab.materialize,
        hasQueryUuid: !!queryUuid,
        hasDataSource: !!dataSource,
      });
      tab.activeHttpDataSource = undefined;
      tab.isLoading = false;
      this.historyRefreshSignal++;
      m.redraw();
    }
  }

  private startPolling(tab: BigTraceEditorTab) {
    if (!tab.queryUuid) {
      console.log('startPolling: no queryUuid for tab', tab.id);
      return;
    }

    console.log(
      'startPolling: starting for tab',
      tab.id,
      'uuid',
      tab.queryUuid,
    );

    const poll = async () => {
      if (!tab.queryUuid) {
        console.log('poll: no queryUuid, stopping');
        return;
      }
      if (!tab.isLoading) {
        console.log('poll: tab is not loading, stopping');
        return;
      }
      try {
        console.log('poll: calling getStatus for', tab.queryUuid);
        const status = await tab.activeHttpDataSource?.getStatus(tab.queryUuid);

        if (!tab.isLoading) {
          console.log('poll: tab became not loading during fetch, stopping');
          return;
        }
        console.log('poll: getStatus response:', JSON.stringify(status));

        if (status !== undefined && status !== null) {
          if (tab.queryUuid !== undefined && tab.queryUuid !== '') {
            queryStore.update(tab.queryUuid, {
              processedRows:
                status.processedRows !== undefined ? status.processedRows : 0,
              processedTraces:
                status.processedTraces !== undefined
                  ? status.processedTraces
                  : 0,
              totalTraces:
                status.totalTraces !== undefined ? status.totalTraces : 0,
              status: status.status || 'N/A',
            });
          }

          // Auto-fetch on first green dot!
          const isTerminal =
            tab.execution?.status === 'SUCCESS' ||
            tab.execution?.status === 'FAILED' ||
            tab.execution?.status === 'CANCELLED';

          if (
            !isTerminal &&
            (tab.execution?.processedRows !== undefined
              ? tab.execution.processedRows
              : 0) > tab.lastProcessedRows
          ) {
            console.log('poll: rows increased, refreshing results!');
            if (tab.dataSource instanceof BigtraceAsyncDataSource) {
              await tab.dataSource.refresh(tab);
              tab.lastProcessedRows =
                tab.execution?.processedRows !== undefined
                  ? tab.execution.processedRows
                  : 0;
            }
          }
          console.log('poll: updated state');
        } else {
          console.error('poll: status is null or undefined');
        }

        if (
          status &&
          (status.status === 'SUCCESS' ||
            status.status === 'FAILED' ||
            status.status === 'CANCELLED')
        ) {
          console.log('poll: terminal status reached:', status.status);
          tab.pollInterval = undefined;

          if (
            tab.execution !== undefined &&
            tab.execution.endTime === undefined &&
            tab.queryUuid !== undefined &&
            tab.queryUuid !== ''
          ) {
            queryStore.update(tab.queryUuid, {endTime: Date.now()});
          }

          // Only refresh history if the query was actively running in the UI!
          if (tab.isLoading) {
            this.historyRefreshSignal++;
          }

          tab.isLoading = false;

          const isFailed = status.status === 'FAILED';
          const isSuccess = status.status === 'SUCCESS';
          const isCancelled = status.status === 'CANCELLED';

          if (isSuccess || isFailed || isCancelled) {
            console.log('poll: terminal state, fetching full details');
            if (isFailed) {
              tab.queryResult = {
                rows: [],
                columns: [],
                error: 'Fetching error details...',
                totalRowCount: 0,
                durationMs:
                  tab.execution?.endTime !== undefined &&
                  tab.execution?.startTime !== undefined
                    ? tab.execution.endTime - tab.execution.startTime
                    : 0,
                statementWithOutputCount: 0,
                statementCount: 1,
                lastStatementSql: tab.editorText,
                query: tab.editorText,
              };
              m.redraw();
            }

            tab.activeHttpDataSource
              ?.getQueryExecution(tab.queryUuid)
              .then((details) => {
                const startTime = details.startTime;
                const endTime = details.endTime;
                if (
                  startTime !== undefined &&
                  endTime !== undefined &&
                  tab.queryUuid
                ) {
                  queryStore.update(tab.queryUuid, {
                    endTime: new Date(endTime).getTime(),
                  });
                }

                if (isFailed && tab.queryResult !== undefined) {
                  tab.queryResult.error =
                    details.errorMessage || 'Query failed';
                  m.redraw();
                }
              })
              .catch((e) => {
                console.error('Failed to fetch query execution details:', e);
                if (isFailed && tab.queryResult !== undefined) {
                  tab.queryResult.error = `Failed to fetch error details: ${e instanceof Error ? e.message : String(e)}`;
                  m.redraw();
                }
              });
          }

          // Auto-fetch only on success!
          if (isSuccess) {
            console.log('poll: success state, auto-fetching results!');
            if (tab.dataSource instanceof BigtraceAsyncDataSource) {
              tab.dataSource.triggerFetch(tab.currentOffset, tab.pageSize);
              tab.lastProcessedRows =
                tab.execution?.processedRows !== undefined
                  ? tab.execution.processedRows
                  : 0;
            }
          }
        } else if (tab.pollInterval !== undefined) {
          console.log('poll: not terminal, scheduling next poll');
          tab.isLoading = true;
          tab.pollInterval = window.setTimeout(poll, 3000);
        }
        m.redraw();
      } catch (e) {
        console.error('poll: failed with error:', e);
        if (tab.pollInterval !== undefined) {
          console.log('poll: error occurred, retrying poll in 1s');
          tab.pollInterval = window.setTimeout(poll, 1000);
        }
        m.redraw();
      }
    };

    if (tab.pollInterval !== undefined) {
      console.log('startPolling: clearing existing interval', tab.pollInterval);
      window.clearTimeout(tab.pollInterval);
    }
    tab.pollInterval = window.setTimeout(poll, 0);
    console.log('startPolling: scheduled timer', tab.pollInterval);
  }

  private async runQueryOnTab(tab: BigTraceEditorTab, query: string) {
    if (!query) return;

    // Abort any in-flight query on this tab.
    tab.activeHttpDataSource?.abort();

    tab.isLoading = true;
    tab.queryResult = undefined;
    tab.lastProcessedRows = 0;
    tab.clientStartTime = Date.now();
    tabsState.markDirty();
    m.redraw();

    if (this.useBigtraceBackend) {
      this.historyRefreshSignal++;
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
        let data: {rows: DataGridRow[]; columns: string[]} = {
          rows: [],
          columns: [],
        };
        if (tab.materialize) {
          data = await httpDataSource.executeAsync();
          if (data.rows.length > 0) {
            const firstRow = data.rows[0];
            const keys = Object.keys(firstRow);
            if (keys.length > 0) {
              tab.queryUuid = String(firstRow[keys[0]]);
              // Update execution reference to use the backend UUID as key
              if (tab.execution) {
                tab.execution = queryStore.getOrCreate(
                  tab.queryUuid,
                  tab.execution,
                );
              }

              // Fetch full details to get start_time!
              try {
                const details = await httpDataSource.getQueryExecution(
                  tab.queryUuid,
                );
                if (details !== undefined && details !== null) {
                  const startTime = details.startTime;
                  if (
                    startTime !== undefined &&
                    tab.queryUuid !== undefined &&
                    tab.queryUuid !== ''
                  ) {
                    queryStore.update(tab.queryUuid, {
                      startTime: new Date(startTime).getTime(),
                    });
                  }
                }
              } catch (e) {
                console.error(
                  'Failed to fetch query details after executeAsync:',
                  e,
                );
              }

              this.startPolling(tab);
            }
          }
          if (tab.queryUuid !== undefined && tab.queryUuid !== '') {
            tab.dataSource = new BigtraceAsyncDataSource(
              tab.queryUuid,
              httpDataSource,
              () =>
                tab.execution?.processedRows !== undefined
                  ? tab.execution.processedRows
                  : 0,
              () => tab.currentOffset,
              () => tab.pageSize,
            );
          }
          tab.queryResult = {
            rows: [],
            columns: [],
            error: undefined,
            totalRowCount: 0,
            durationMs: performance.now() - startMs,
            statementWithOutputCount: 0,
            statementCount: 1,
            lastStatementSql: query,
            query,
          };
        } else {
          const result = await httpDataSource.query();
          tab.queryResult = {
            rows: result.rows,
            columns: result.columns,
            error: undefined,
            totalRowCount: result.rows.length,
            durationMs: performance.now() - startMs,
            statementWithOutputCount: 1,
            statementCount: 1,
            lastStatementSql: query,
            query,
          };
          queryStore.update(tab.queryUuid || tab.id, {
            processedRows: result.rows.length,
          });
          tab.isLoading = false;
        }
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
        if (!tab.materialize) {
          tab.activeHttpDataSource = undefined;
        }
      }
    } else {
      throw new Error(
        'Local query execution is unsupported in bigtrace context.',
      );
    }

    if (tab.queryResult !== undefined && !tab.materialize) {
      tab.dataSource = new InMemoryDataSource(tab.queryResult.rows);
    }

    if (!tab.materialize) {
      tab.isLoading = false;
    }
    m.redraw();
  }

  private renderQueryResult(
    queryResult: QueryResponse,
    dataSource: DataSource,
    querySettings: SettingFilter[],
    tab: BigTraceEditorTab,
  ) {
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

    const isTerminal =
      tab.execution?.status === 'SUCCESS' ||
      tab.execution?.status === 'FAILED' ||
      tab.execution?.status === 'CANCELLED';

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
        let columns = queryResult.columns;
        if (
          columns.length === 0 &&
          dataSource instanceof BigtraceAsyncDataSource
        ) {
          const cols = dataSource.getColumns();
          columns = cols !== undefined ? cols : [];
        }

        if (dataSource instanceof BigtraceAsyncDataSource) {
          const error = dataSource.getError();
          if (
            error !== null &&
            error !== '' &&
            (isTerminal || error.includes('status: 400') === false)
          ) {
            return m(EmptyState, {
              title: `Failed to load schema: ${error}`,
              icon: 'error',
              fillHeight: true,
            });
          }
        }

        if (columns.length === 0) {
          // Trigger useRows to start fetching data and columns!
          dataSource.useRows({mode: 'flat', columns: []});

          return m(
            EmptyState,
            {
              title: 'Loading schema...',
              icon: 'hourglass_empty',
              fillHeight: true,
            },
            m(Spinner),
          );
        }

        // Build schema directly
        const columnSchema: ColumnSchema = {};
        for (const column of columns) {
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
          initialColumns: columns
            .filter((col) => {
              if (!col.startsWith('_')) return true;
              if (col === '_trace_id') return true;
              const settingId = col.substring(1);
              return querySettings.some(
                (s) =>
                  s.settingId === settingId && s.category === 'TRACE_METADATA',
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
            tab.materialize
              ? (() => {
                  const start = tab.currentOffset + 1;
                  const end = Math.min(
                    tab.currentOffset + tab.pageSize,
                    tab.execution?.processedRows !== undefined
                      ? tab.execution.processedRows
                      : 0,
                  );
                  const isTerminal =
                    tab.execution?.status === 'SUCCESS' ||
                    tab.execution?.status === 'FAILED' ||
                    tab.execution?.status === 'CANCELLED';
                  if (isTerminal) {
                    return `Showing ${start}-${end} of ${(tab.execution?.processedRows !== undefined ? tab.execution.processedRows : 0).toLocaleString()} rows`;
                  } else {
                    return `Showing ${start}-${end} rows`;
                  }
                })()
              : `Returned ${queryResult.totalRowCount.toLocaleString()} rows in ${Math.round(queryResult.durationMs).toLocaleString()} ms`,
          ),
          toolbarItemsRight: [
            tab.materialize &&
              m(
                Select,
                {
                  value: String(tab.pageSize),
                  onchange: (e: Event) => {
                    const newPageSize = Number(
                      (e.target as HTMLSelectElement).value,
                    );
                    tab.currentOffset =
                      Math.floor(tab.currentOffset / newPageSize) * newPageSize;
                    tab.pageSize = newPageSize;
                    tabsState.globalPageSize = newPageSize;
                    tabsState.markDirty();
                    if (dataSource instanceof BigtraceAsyncDataSource) {
                      dataSource.triggerFetch(tab.currentOffset, tab.pageSize);
                    }
                  },
                },
                [
                  m('option', {value: '50'}, '50'),
                  m('option', {value: '100'}, '100'),
                  m('option', {value: '250'}, '250'),
                ],
              ),
            tab.materialize &&
              m(Button, {
                icon: 'arrow_back',
                title: 'Previous page',
                disabled: tab.currentOffset === 0,
                onclick: () => {
                  tab.currentOffset = Math.max(
                    0,
                    tab.currentOffset - tab.pageSize,
                  );
                  if (dataSource instanceof BigtraceAsyncDataSource) {
                    dataSource.triggerFetch(tab.currentOffset, tab.pageSize);
                  }
                },
              }),
            tab.materialize &&
              m(Button, {
                icon: 'arrow_forward',
                title: 'Next page',
                disabled:
                  tab.currentOffset + tab.pageSize >=
                  (tab.execution?.processedRows !== undefined
                    ? tab.execution.processedRows
                    : 0),
                onclick: () => {
                  tab.currentOffset += tab.pageSize;
                  if (dataSource instanceof BigtraceAsyncDataSource) {
                    dataSource.triggerFetch(tab.currentOffset, tab.pageSize);
                  }
                },
              }),
            m(CopyToClipboardButton, {
              textToCopy: queryResult.query,
              title: 'Copy executed query to clipboard',
              label: 'Copy Query',
            }),
          ].filter(Boolean) as m.Children[],
        });
      })(),
    ];

    return m('.pf-query-page__results', [
      m(Tabs, {
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
      }),
    ]);
  }
}
