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
import {Tabs, TabsTab} from '../../widgets/tabs';
import {Stack, StackAuto} from '../../widgets/stack';
import {Anchor} from '../../widgets/anchor';
import {DataSource} from '../../components/widgets/datagrid/data_source';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {TableList} from './table_list';
import {ResultsTable} from './results_table';

const HIDE_PERFETTO_SQL_AGENT_BANNER_KEY = 'hidePerfettoSqlAgentBanner';

// Represents a single query editor tab with its own state.
export interface QueryEditorTab {
  readonly id: string;
  editorText: string;
  queryResult?: QueryResponse;
  isLoading: boolean;
  title: string;
}

export interface QueryPageAttrs {
  // The trace to run queries against.
  readonly trace: Trace;

  // All editor tabs.
  readonly editorTabs: QueryEditorTab[];

  // The currently active editor tab ID.
  readonly activeTabId: string;

  // Called when the content of an editor is updated.
  onEditorContentUpdate?(tabId: string, content: string): void;

  // Called when the user requests to execute a query.
  onExecute?(tabId: string, query: string): void;

  // Called when the user switches to a different tab.
  onTabChange?(tabId: string): void;

  // Called when the user closes a tab.
  onTabClose?(tabId: string): void;

  // Called when the user wants to add a new tab.
  onTabAdd?(
    tabName?: string,
    initialQuery?: string,
    autoExecute?: boolean,
  ): void;

  // Called when the user renames a tab.
  onTabRename?(tabId: string, newName: string): void;

  // Called when the user reorders tabs via drag and drop.
  // draggedTabId is the tab being moved, beforeTabId is the tab it should be
  // placed before (or undefined if moved to the end).
  onTabReorder?(draggedTabId: string, beforeTabId: string | undefined): void;
}

export class QueryPage implements m.ClassComponent<QueryPageAttrs> {
  // Map of tab ID to DataSource for each tab's query results
  private dataSources = new Map<string, DataSource>();

  // Track previous query results to detect changes
  private prevQueryResults = new Map<string, QueryResponse | undefined>();

  view({attrs}: m.CVnode<QueryPageAttrs>) {
    const {editorTabs, activeTabId} = attrs;

    // Update data sources for tabs whose results have changed
    for (const tab of editorTabs) {
      const prevResult = this.prevQueryResults.get(tab.id);
      if (tab.queryResult !== prevResult) {
        if (tab.queryResult) {
          this.dataSources.set(
            tab.id,
            new InMemoryDataSource(tab.queryResult.rows),
          );
        } else {
          this.dataSources.delete(tab.id);
        }
        this.prevQueryResults.set(tab.id, tab.queryResult);
      }
    }

    // Clean up data sources for removed tabs
    const tabIds = new Set(editorTabs.map((t) => t.id));
    for (const id of this.dataSources.keys()) {
      if (!tabIds.has(id)) {
        this.dataSources.delete(id);
        this.prevQueryResults.delete(id);
      }
    }

    // Build editor tabs for the left panel
    const leftTabs: TabsTab[] = editorTabs.map((tab) => ({
      key: tab.id,
      title: tab.title,
      leftIcon: 'code',
      closeButton: editorTabs.length > 1,
      content: this.renderEditorTabContent(attrs, tab),
    }));

    const leftPanel = m(Tabs, {
      className: 'pf-query-page__editor-tabs',
      tabs: leftTabs,
      activeTabKey: activeTabId,
      reorderable: true,
      onTabChange: (key) => attrs.onTabChange?.(key),
      onTabRename: (key, newTitle) => {
        attrs.onTabRename?.(key, newTitle);
      },
      onTabClose: (key) => attrs.onTabClose?.(key),
      onTabReorder: (draggedKey, beforeKey) =>
        attrs.onTabReorder?.(draggedKey, beforeKey),
      onNewTab: () => attrs.onTabAdd?.(),
    });

    const activeTab = editorTabs.find((t) => t.id === activeTabId);

    const sidebarPanel = m(Tabs, {
      className: 'pf-query-page__sidebar',
      tabs: [
        {
          key: 'history',
          title: 'History',
          leftIcon: 'history',
          content: m(QueryHistoryComponent, {
            className: 'pf-query-page__history',
            trace: attrs.trace,
            runQuery: (query: string) => {
              if (activeTab) {
                attrs.onExecute?.(activeTab.id, query);
              }
            },
            setQuery: (query: string) => {
              if (activeTab) {
                attrs.onEditorContentUpdate?.(activeTab.id, query);
              }
            },
          }),
        },
        {
          key: 'tables',
          title: 'Tables',
          leftIcon: 'table_chart',
          content: this.renderTablesTab(attrs),
        },
      ],
    });

    return m(
      '.pf-query-page',
      m(SplitPanel, {
        direction: 'horizontal',
        initialSplit: {pixels: 500},
        controlledPanel: 'second',
        minSize: 100,
        firstPanel: leftPanel,
        secondPanel: sidebarPanel,
      }),
    );
  }

  private renderEditorTabContent(
    attrs: QueryPageAttrs,
    tab: QueryEditorTab,
  ): m.Children {
    const {trace} = attrs;
    const dataSource = this.dataSources.get(tab.id);

    const editorPanel = m('.pf-query-page__editor-panel', [
      m(Box, {className: 'pf-query-page__toolbar'}, [
        m(Stack, {orientation: 'horizontal'}, [
          m(Button, {
            label: 'Run Query',
            icon: 'play_arrow',
            loading: tab.isLoading,
            intent: tab.isLoading ? Intent.None : Intent.Primary,
            variant: ButtonVariant.Filled,
            onclick: () => {
              attrs.onExecute?.(tab.id, tab.editorText);
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
          m(StackAuto), // The spacer pushes the following buttons to the right.
          trace.isInternalUser &&
            m(Button, {
              icon: 'wand_stars',
              title:
                'Generate SQL queries with the Perfetto SQL Agent! Give feedback: go/perfetto-llm-bug',
              label: 'Generate SQL Queries with AI',
              onclick: () => {
                window.open('http://go/perfetto-sql-agent', '_blank');
              },
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
        onUpdate: (content) => attrs.onEditorContentUpdate?.(tab.id, content),
        onExecute: (query) => attrs.onExecute?.(tab.id, query),
      }),
    ]);

    const resp = tab.queryResult;
    const resultsPanel =
      resp &&
      m(ResultsTable, {
        data: resp.error
          ? {kind: 'error', errorMessage: resp.error}
          : {
              kind: 'success',
              columns: resp.columns,
              rows: resp.rows,
              dataSource: dataSource!,
              rowCount: resp.totalRowCount,
              queryTimeMs: resp.durationMs,
              query: resp.query,
              lastStatementSql: resp.lastStatementSql,
              statementCount: resp.statementCount,
              statementWithOutputCount: resp.statementWithOutputCount,
            },
        fillHeight: true,
        trace,
        onIdClick: (sqlTable, id, doubleClick) => {
          trace.navigate('#!/viewer');
          trace.selection.selectSqlEvent(sqlTable, id, {
            switchToCurrentSelectionTab: doubleClick,
            scrollToSelection: true,
          });
        },
      });

    return m(SplitPanel, {
      direction: 'vertical',
      initialSplit: {percent: 50},
      minSize: 100,
      firstPanel: editorPanel,
      secondPanel: resultsPanel,
    });
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

    return m(TableList, {
      sqlModules,
      onQueryTable: (tableName, query) => {
        attrs.onTabAdd?.(tableName, query, true);
      },
    });
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
