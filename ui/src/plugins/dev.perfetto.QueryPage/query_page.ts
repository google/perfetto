// Copyright (C) 2025 The Android Open Source Project
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
import {QueryResponse} from '../../components/query_table/queries';
import {DataGrid, renderCell} from '../../components/widgets/datagrid/datagrid';
import {
  CellRenderer,
  ColumnSchema,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {QueryHistoryComponent} from '../../components/widgets/query_history';
import {Trace} from '../../public/trace';
import {Box} from '../../widgets/box';
import {Button, ButtonVariant} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {Editor} from '../../widgets/editor';
import {EmptyState} from '../../widgets/empty_state';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {Spinner} from '../../widgets/spinner';
import {SplitPanel} from '../../widgets/split_panel';
import {Tabs} from '../../widgets/tabs';
import {Stack, StackAuto} from '../../widgets/stack';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {Anchor} from '../../widgets/anchor';
import {getSliceId, isSliceish} from '../../components/query_table/query_table';
import {DataSource} from '../../components/widgets/datagrid/data_source';
import {Row} from '../../trace_processor/query_result';
import {PopupMenu} from '../../widgets/menu';
import {PopupPosition} from '../../widgets/popup';
import {AddDebugTrackMenu} from '../../components/tracks/add_debug_track_menu';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {SimpleTableList} from './simple_table_list';

const HIDE_PERFETTO_SQL_AGENT_BANNER_KEY = 'hidePerfettoSqlAgentBanner';

export interface QueryTabState {
  id: string;
  editorText: string;
  executedQuery?: string;
  queryResult?: QueryResponse;
}

export interface QueryPageAttrs {
  readonly trace: Trace;
  readonly tabs: QueryTabState[];
  readonly activeTabId: string;
  readonly isLoading: boolean;

  onTabChange(tabId: string): void;
  onAddTab(): void;
  onCloseTab(tabId: string): void;
  onEditorContentUpdate(tabId: string, content: string): void;
  onExecute(query: string): void;
}

interface DataSourceInfo {
  dataSource: DataSource;
  sourceRows: readonly Row[];
}

export class QueryPage implements m.ClassComponent<QueryPageAttrs> {
  // Map of tab ID to data source info (including source rows for comparison)
  private dataSourceInfo = new Map<string, DataSourceInfo>();

  view({attrs}: m.CVnode<QueryPageAttrs>) {
    const activeTab = attrs.tabs.find((t) => t.id === attrs.activeTabId);

    // Build tabs for the left panel
    const queryTabs = attrs.tabs.map((tab, index) => ({
      key: tab.id,
      title: `Query ${index + 1}`,
      icon: 'code',
      closable: attrs.tabs.length > 1,
      content: this.renderQueryTab(attrs, tab),
    }));

    const leftPanel = m(Tabs, {
      className: 'pf-query-page__query-tabs',
      tabs: queryTabs,
      activeTabKey: attrs.activeTabId,
      onTabChange: attrs.onTabChange,
      onTabClose: attrs.onCloseTab,
      onAddTab: attrs.onAddTab,
    });

    const sidebarPanel = m(Tabs, {
      className: 'pf-query-page__sidebar',
      tabs: [
        {
          key: 'history',
          title: 'History',
          icon: 'history',
          content: m(QueryHistoryComponent, {
            className: 'pf-query-page__history',
            trace: attrs.trace,
            runQuery: (query: string) => {
              attrs.onExecute(query);
            },
            setQuery: (query: string) => {
              if (activeTab) {
                attrs.onEditorContentUpdate(activeTab.id, query);
              }
            },
          }),
        },
        {
          key: 'tables',
          title: 'Tables',
          icon: 'table',
          content: this.renderTablesTab(attrs),
        },
      ],
    });

    return m(
      '.pf-query-page',
      m(SplitPanel, {
        direction: 'horizontal',
        split: {percent: 70},
        minSize: 100,
        firstPanel: leftPanel,
        secondPanel: sidebarPanel,
      }),
    );
  }

  private renderQueryTab(
    attrs: QueryPageAttrs,
    tab: QueryTabState,
  ): m.Children {
    // Get or create data source for this tab
    let info = this.dataSourceInfo.get(tab.id);
    let dataSource = info?.dataSource;
    if (tab.queryResult && tab.queryResult.rows) {
      // Check if we need to update the data source
      if (!info || info.sourceRows !== tab.queryResult.rows) {
        dataSource = new InMemoryDataSource(tab.queryResult.rows);
        this.dataSourceInfo.set(tab.id, {
          dataSource,
          sourceRows: tab.queryResult.rows,
        });
      }
    } else if (info && !tab.queryResult) {
      this.dataSourceInfo.delete(tab.id);
      dataSource = undefined;
    }

    const editorPanel = m('.pf-query-page__editor-panel', [
      m(Box, {className: 'pf-query-page__toolbar'}, [
        m(Stack, {orientation: 'horizontal'}, [
          m(Button, {
            label: 'Run Query',
            icon: 'play_arrow',
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            onclick: () => {
              attrs.onExecute(tab.editorText);
            },
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
          attrs.trace.isInternalUser &&
            m(Button, {
              icon: 'wand_stars',
              title:
                'Generate SQL queries with the Perfetto SQL Agent! Give feedback: go/perfetto-llm-bug',
              label: 'Generate SQL Queries with AI',
              onclick: () => {
                window.open('http://go/perfetto-sql-agent', '_blank');
              },
            }),
          m(CopyToClipboardButton, {
            textToCopy: tab.editorText,
            title: 'Copy query to clipboard',
            label: 'Copy Query',
          }),
        ]),
      ]),
      this.shouldDisplayPerfettoSqlAgentBanner(attrs) &&
        m(
          Box,
          m(
            Callout,
            {
              icon: 'wand_stars',
              dismissible: true,
              onDismiss: () => {
                this.hidePerfettoSqlAgentBanner();
              },
            },
            [
              'Try out the ',
              m(
                Anchor,
                {
                  href: 'http://go/perfetto-sql-agent',
                  target: '_blank',
                  icon: Icons.ExternalLink,
                },
                'Perfetto SQL Agent',
              ),
              ' to generate SQL queries and ',
              m(
                Anchor,
                {
                  href: 'http://go/perfetto-llm-user-guide#report-issues',
                  target: '_blank',
                  icon: Icons.ExternalLink,
                },
                'give feedback',
              ),
              '!',
            ],
          ),
        ),
      tab.editorText.includes('"') &&
        m(
          Box,
          m(
            Callout,
            {icon: 'warning', intent: Intent.None},
            `" (double quote) character observed in query; if this is being used to ` +
              `define a string, please use ' (single quote) instead. Using double quotes ` +
              `can cause subtle problems which are very hard to debug.`,
          ),
        ),
      m(Editor, {
        language: 'perfetto-sql',
        text: tab.editorText,
        onUpdate: (text) => attrs.onEditorContentUpdate(tab.id, text),
        onExecute: attrs.onExecute,
      }),
    ]);

    const resultsPanel = m(
      '.pf-query-page__results-panel',
      dataSource && tab.queryResult
        ? this.renderQueryResult(attrs.trace, tab.queryResult, dataSource)
        : tab.executedQuery && attrs.isLoading && tab.id === attrs.activeTabId
          ? m(EmptyState, {
              title: 'Running query...',
              icon: 'hourglass_empty',
              fillHeight: true,
            })
          : m(EmptyState, {
              title: 'Run a query to see results',
              fillHeight: true,
            }),
    );

    return m(SplitPanel, {
      direction: 'vertical',
      split: {percent: 50},
      minSize: 100,
      firstPanel: editorPanel,
      secondPanel: resultsPanel,
    });
  }

  private renderQueryResult(
    trace: Trace,
    queryResult: QueryResponse,
    dataSource: DataSource,
  ) {
    const queryTimeString = `${queryResult.durationMs.toFixed(1)} ms`;
    if (queryResult.error) {
      return m(
        '.pf-query-page__query-error',
        `SQL error: ${queryResult.error}`,
      );
    } else {
      return [
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
            const cellRenderer: CellRenderer | undefined =
              column === 'id'
                ? (value, row) => {
                    const sliceId = getSliceId(row);
                    const cell = renderCell(value, column);
                    if (sliceId !== undefined && isSliceish(row)) {
                      return m(
                        Anchor,
                        {
                          title: 'Go to slice on the timeline',
                          icon: Icons.UpdateSelection,
                          onclick: () => {
                            // Navigate to the timeline page
                            trace.navigate('#!/viewer');
                            trace.selection.selectSqlEvent('slice', sliceId, {
                              switchToCurrentSelectionTab: false,
                              scrollToSelection: true,
                            });
                          },
                        },
                        cell,
                      );
                    } else {
                      return renderCell(value, column);
                    }
                  }
                : undefined;
            columnSchema[column] = {cellRenderer};
          }
          const schema: SchemaRegistry = {data: columnSchema};
          const lastStatement = queryResult.lastStatementSql;

          return m(DataGrid, {
            schema,
            rootSchema: 'data',
            initialColumns: queryResult.columns.map((col) => ({field: col})),
            className: 'pf-query-page__results',
            data: dataSource,
            showExportButton: true,
            toolbarItemsLeft: m(
              'span.pf-query-page__results-summary',
              `Returned ${queryResult.totalRowCount.toLocaleString()} rows in ${queryTimeString}`,
            ),
            toolbarItemsRight: [
              m(
                PopupMenu,
                {
                  trigger: m(Button, {label: 'Add debug track'}),
                  position: PopupPosition.Top,
                },
                m(AddDebugTrackMenu, {
                  trace,
                  query: lastStatement,
                  availableColumns: queryResult.columns,
                  onAdd: () => {
                    // Navigate to the tracks page
                    trace.navigate('#!/viewer');
                  },
                }),
              ),
              m(CopyToClipboardButton, {
                textToCopy: queryResult.query,
                title: 'Copy executed query to clipboard',
                label: 'Copy Query',
              }),
            ],
          });
        })(),
      ];
    }
  }

  private renderTablesTab(attrs: QueryPageAttrs): m.Children {
    const sqlModulesPlugin = attrs.trace.plugins.getPlugin(SqlModulesPlugin);
    const sqlModules = sqlModulesPlugin.getSqlModules();

    if (!sqlModules) {
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

    return m(SimpleTableList, {sqlModules});
  }

  private shouldDisplayPerfettoSqlAgentBanner(attrs: QueryPageAttrs) {
    return (
      attrs.trace.isInternalUser &&
      localStorage.getItem(HIDE_PERFETTO_SQL_AGENT_BANNER_KEY) !== 'true'
    );
  }

  private hidePerfettoSqlAgentBanner() {
    localStorage.setItem(HIDE_PERFETTO_SQL_AGENT_BANNER_KEY, 'true');
  }
}
