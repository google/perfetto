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
import {z} from 'zod';
import {
  QueryResponse,
  runQueryForQueryTable,
} from '../../components/query_table/queries';
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

// Schema for a single tab's persistent state (what goes in permalinks)
const SerializedTabSchema = z.object({
  id: z.string(),
  title: z.string(),
  editorText: z.string(),
});

type SerializedTab = z.infer<typeof SerializedTabSchema>;

// Schema for the full plugin state
const queryPageStateSchema = z.object({
  tabs: z.array(SerializedTabSchema).default([]),
  activeTabId: z.string().optional(),
});

// Transient state (not persisted) - query results and loading state per tab
interface TransientTabState {
  queryResult?: QueryResponse;
  isLoading: boolean;
}

export default class QueryPagePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.QueryPage';
  static readonly dependencies = [SqlModulesPlugin];

  static addQueryPageMiniFlag: Flag;

  // Transient state keyed by tab ID - not persisted in permalinks
  private transientState = new Map<string, TransientTabState>();

  async onTraceLoad(trace: Trace): Promise<void> {
    const store = trace.mountStore(QueryPagePlugin.id, queryPageStateSchema);
    this.transientState.clear();

    const createNewTab = (
      tabName?: string,
      editorText: string = '',
    ): SerializedTab => {
      // If no tab name is provided, generate a unique one
      if (!tabName) {
        const existingNames = new Set(store.state.tabs.map((t) => t.title));
        let count = 1;
        while (existingNames.has(`Query ${count}`)) {
          count++;
        }
        tabName = `Query ${count}`;
      }

      return {
        id: shortUuid(),
        title: tabName,
        editorText,
      };
    };

    // Initialize with one tab if empty
    if (store.state.tabs.length === 0) {
      const initialTab = createNewTab();
      store.edit((draft) => {
        draft.tabs.push(initialTab);
        draft.activeTabId = initialTab.id;
      });
    }

    // Initialize transient state for all tabs
    for (const tab of store.state.tabs) {
      this.transientState.set(tab.id, {isLoading: false});
    }

    // Build QueryEditorTab array from store + transient state
    const getEditorTabs = (): QueryEditorTab[] => {
      return store.state.tabs.map((tab) => {
        const transient = this.transientState.get(tab.id) ?? {isLoading: false};
        return {
          id: tab.id,
          title: tab.title,
          editorText: tab.editorText,
          queryResult: transient.queryResult,
          isLoading: transient.isLoading,
        };
      });
    };

    const getActiveTabId = (): string => {
      return store.state.activeTabId ?? store.state.tabs[0]?.id;
    };

    const getActiveTab = (): QueryEditorTab | undefined => {
      const tabs = getEditorTabs();
      return tabs.find((t) => t.id === getActiveTabId());
    };

    const onExecute = async (tabId: string, text: string) => {
      if (!text) return;

      const transient = this.transientState.get(tabId);
      if (!transient) return;

      transient.queryResult = undefined;
      queryHistoryStorage.saveQuery(text);

      transient.isLoading = true;
      transient.queryResult = await runQueryForQueryTable(text, trace.engine);
      transient.isLoading = false;

      trace.tabs.showTab('dev.perfetto.QueryPage');
    };

    const onEditorContentUpdate = (tabId: string, content: string) => {
      store.edit((draft) => {
        const tab = draft.tabs.find((t) => t.id === tabId);
        if (tab) {
          tab.editorText = content;
        }
      });
    };

    const onTabChange = (tabId: string) => {
      store.edit((draft) => {
        draft.activeTabId = tabId;
      });
    };

    const onTabClose = (tabId: string) => {
      const tabs = store.state.tabs;
      const index = tabs.findIndex((t) => t.id === tabId);
      if (index === -1) return;

      // Don't close the last tab
      if (tabs.length === 1) return;

      store.edit((draft) => {
        draft.tabs.splice(index, 1);

        // If we closed the active tab, switch to an adjacent one
        if (draft.activeTabId === tabId) {
          const newIndex = Math.min(index, draft.tabs.length - 1);
          draft.activeTabId = draft.tabs[newIndex].id;
        }
      });

      this.transientState.delete(tabId);
    };

    const onTabAdd = (
      tabName?: string,
      initialQuery?: string,
      autoExecute?: boolean,
    ) => {
      const newTab = createNewTab(tabName, initialQuery);
      store.edit((draft) => {
        draft.tabs.push(newTab);
        draft.activeTabId = newTab.id;
      });
      this.transientState.set(newTab.id, {isLoading: false});

      if (autoExecute) {
        onExecute(newTab.id, initialQuery ?? '');
      }
    };

    const onTabRename = (tabId: string, newName: string) => {
      store.edit((draft) => {
        const tab = draft.tabs.find((t) => t.id === tabId);
        if (tab) {
          tab.title = newName;
        }
      });
    };

    trace.pages.registerPage({
      route: '/query',
      render: () =>
        m(QueryPage, {
          trace,
          editorTabs: getEditorTabs(),
          activeTabId: getActiveTabId(),
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
