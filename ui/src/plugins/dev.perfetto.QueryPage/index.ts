// Copyright (C) 2024 The Android Open Source Project
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
import {runQueryForQueryTable} from '../../components/query_table/queries';
import {QueryResultsTable} from '../../components/query_table/query_table';
import {Flag} from '../../public/feature_flag';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {QueryPage, QueryEditorTab} from './query_page';
import {queryHistoryStorage} from '../../components/widgets/query_history';
import {EmptyState} from '../../widgets/empty_state';
import {Anchor} from '../../widgets/anchor';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {shortUuid} from '../../base/uuid';

export default class QueryPagePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.QueryPage';
  static readonly dependencies = [SqlModulesPlugin];

  static addQueryPageMiniFlag: Flag;

  async onTraceLoad(trace: Trace): Promise<void> {
    // Multi-tab state: array of editor tabs with active tab tracking
    const editorTabs: QueryEditorTab[] = [];

    function createNewTabName(index: number): string {
      return `Query ${index}`;
    }

    function createNewTab(
      tabName?: string,
      editorText: string = '',
    ): QueryEditorTab {
      // If no tab name is provided, count up until we find a unique name
      if (!tabName) {
        let count = 1;
        const existingNames = new Set<string>();
        // This function is only called during initialization, so we can
        // safely access the existing tabs from the closure.
        for (const tab of editorTabs) {
          existingNames.add(tab.title);
        }
        while (existingNames.has(createNewTabName(count))) {
          count++;
        }
        tabName = createNewTabName(count);
      }

      return {
        id: shortUuid(),
        editorText,
        queryResult: undefined,
        isLoading: false,
        title: tabName,
      };
    }

    editorTabs.push(createNewTab());
    let activeTabId = editorTabs[0].id;

    // Helper to find the active tab
    function getActiveTab(): QueryEditorTab | undefined {
      return editorTabs.find((t) => t.id === activeTabId);
    }

    async function onExecute(tabId: string, text: string) {
      if (!text) return;

      const tab = editorTabs.find((t) => t.id === tabId);
      if (!tab) return;

      tab.queryResult = undefined;
      queryHistoryStorage.saveQuery(text);

      tab.isLoading = true;
      tab.queryResult = await runQueryForQueryTable(text, trace.engine);
      tab.isLoading = false;

      trace.tabs.showTab('dev.perfetto.QueryPage');
    }

    function onEditorContentUpdate(tabId: string, content: string) {
      const tab = editorTabs.find((t) => t.id === tabId);
      if (tab) {
        tab.editorText = content;
      }
    }

    function onTabChange(tabId: string) {
      activeTabId = tabId;
    }

    function onTabClose(tabId: string) {
      const index = editorTabs.findIndex((t) => t.id === tabId);
      if (index === -1) return;

      // Don't close the last tab
      if (editorTabs.length === 1) return;

      editorTabs.splice(index, 1);

      // If we closed the active tab, switch to an adjacent one
      if (activeTabId === tabId) {
        const newIndex = Math.min(index, editorTabs.length - 1);
        activeTabId = editorTabs[newIndex].id;
      }
    }

    function onTabAdd(
      tabName?: string,
      initialQuery?: string,
      autoExecute?: boolean,
    ) {
      const newTab = createNewTab(tabName, initialQuery);
      editorTabs.push(newTab);
      activeTabId = newTab.id;

      if (autoExecute) {
        onExecute(newTab.id, initialQuery ?? '');
      }
    }

    function onTabRename(tabId: string, newName: string) {
      const tab = editorTabs.find((t) => t.id === tabId);
      if (tab) {
        tab.title = newName;
      }
    }

    trace.pages.registerPage({
      route: '/query',
      render: () =>
        m(QueryPage, {
          trace,
          editorTabs,
          activeTabId,
          onEditorContentUpdate,
          onExecute,
          onTabChange,
          onTabClose,
          onTabAdd,
          onTabRename,
        }),
    });

    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Query (SQL)',
      href: '#!/query',
      icon: 'database',
      sortOrder: 21,
    });

    trace.tabs.registerTab({
      uri: 'dev.perfetto.QueryPage',
      isEphemeral: false,
      content: {
        render() {
          const activeTab = getActiveTab();
          return m(QueryResultsTable, {
            trace,
            isLoading: activeTab?.isLoading ?? false,
            resp: activeTab?.queryResult,
            fillHeight: true,
            emptyState: m(
              EmptyState,
              {
                fillHeight: true,
                title: 'No query results',
              },
              [
                'Execute a query in the ',
                m(Anchor, {href: '#!/query'}, 'Query Page'),
                ' to see results here.',
              ],
            ),
          });
        },
        getTitle() {
          return 'Query Page Results';
        },
      },
    });
  }
}
