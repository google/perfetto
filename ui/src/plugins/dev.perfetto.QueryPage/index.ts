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
import {
  QueryResponse,
  runQueryForQueryTable,
} from '../../components/query_table/queries';
import {QueryResultsTable} from '../../components/query_table/query_table';
import {Flag} from '../../public/feature_flag';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {QueryPage, QueryTabState} from './query_page';
import {queryHistoryStorage} from '../../components/widgets/query_history';
import {EmptyState} from '../../widgets/empty_state';
import {Anchor} from '../../widgets/anchor';

let nextTabId = 1;

function createNewTab(): QueryTabState {
  return {
    id: `query-${nextTabId++}`,
    editorText: '',
    executedQuery: undefined,
    queryResult: undefined,
  };
}

export default class QueryPagePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.QueryPage';
  static addQueryPageMiniFlag: Flag;

  async onTraceLoad(trace: Trace): Promise<void> {
    // State for multiple query tabs
    const tabs: QueryTabState[] = [createNewTab()];
    let activeTabId = tabs[0].id;
    let isLoading = false;

    function getActiveTab(): QueryTabState | undefined {
      return tabs.find((t) => t.id === activeTabId);
    }

    async function onExecute(text: string) {
      if (!text) return;

      const tab = getActiveTab();
      if (!tab) return;

      tab.executedQuery = text;
      tab.queryResult = undefined;
      queryHistoryStorage.saveQuery(text);

      isLoading = true;
      m.redraw();

      tab.queryResult = await runQueryForQueryTable(text, trace.engine);
      isLoading = false;

      trace.tabs.showTab('dev.perfetto.QueryPage');
    }

    function onAddTab() {
      const newTab = createNewTab();
      tabs.push(newTab);
      activeTabId = newTab.id;
    }

    function onCloseTab(tabId: string) {
      const index = tabs.findIndex((t) => t.id === tabId);
      if (index === -1) return;

      // Don't close the last tab
      if (tabs.length === 1) return;

      tabs.splice(index, 1);

      // If we closed the active tab, switch to another
      if (activeTabId === tabId) {
        activeTabId = tabs[Math.min(index, tabs.length - 1)].id;
      }
    }

    function onTabChange(tabId: string) {
      activeTabId = tabId;
    }

    function onEditorContentUpdate(tabId: string, text: string) {
      const tab = tabs.find((t) => t.id === tabId);
      if (tab) {
        tab.editorText = text;
      }
    }

    trace.pages.registerPage({
      route: '/query',
      render: () =>
        m(QueryPage, {
          trace,
          tabs,
          activeTabId,
          isLoading,
          onTabChange,
          onAddTab,
          onCloseTab,
          onEditorContentUpdate,
          onExecute,
        }),
    });

    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Query (SQL)',
      href: '#!/query',
      icon: 'database',
      sortOrder: 20,
    });

    trace.tabs.registerTab({
      uri: 'dev.perfetto.QueryPage',
      isEphemeral: false,
      content: {
        render() {
          const activeTab = getActiveTab();
          return m(QueryResultsTable, {
            trace,
            isLoading,
            query: activeTab?.executedQuery,
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
