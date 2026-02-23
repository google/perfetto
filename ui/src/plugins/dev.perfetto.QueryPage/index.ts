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
import {runQueryForQueryTable} from '../../components/query_table/queries';
import {QueryResultsTable} from '../../components/query_table/query_table';
import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {Setting} from '../../public/settings';
import {Trace} from '../../public/trace';
import {QueryPage, QueryEditorTab} from './query_page';
import {queryHistoryStorage} from '../../components/widgets/query_history';
import {EmptyState} from '../../widgets/empty_state';
import {Anchor} from '../../widgets/anchor';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {shortUuid} from '../../base/uuid';
import {debounce} from '../../base/rate_limiters';

const QUERY_TABS_STORAGE_KEY = 'perfettoQueryTabs';

const persistedTabSchema = z.object({
  id: z.string(),
  editorText: z.string(),
  title: z.string(),
});

const persistedTabStateSchema = z.object({
  tabs: z.array(persistedTabSchema).min(1),
  activeTabId: z.string(),
});

type PersistedTabState = z.infer<typeof persistedTabStateSchema>;

function saveTabsToStorage(
  setting: Setting<boolean>,
  tabs: QueryEditorTab[],
  activeTabId: string,
): void {
  if (!setting.get()) return;

  const state: PersistedTabState = {
    tabs: tabs.map((tab) => ({
      id: tab.id,
      editorText: tab.editorText,
      title: tab.title,
    })),
    activeTabId,
  };
  localStorage.setItem(QUERY_TABS_STORAGE_KEY, JSON.stringify(state));
}

function loadTabsFromStorage(
  setting: Setting<boolean>,
): PersistedTabState | undefined {
  if (!setting.get()) return undefined;

  const stored = localStorage.getItem(QUERY_TABS_STORAGE_KEY);
  if (!stored) return undefined;

  try {
    const parsed = JSON.parse(stored);
    const result = persistedTabStateSchema.safeParse(parsed);
    if (!result.success) {
      return undefined;
    }
    return result.data;
  } catch {
    return undefined;
  }
}

export default class QueryPagePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.QueryPage';
  static readonly dependencies = [SqlModulesPlugin];

  private static queryTabPersistenceSetting: Setting<boolean>;

  static onActivate(app: App): void {
    QueryPagePlugin.queryTabPersistenceSetting = app.settings.register({
      id: `${QueryPagePlugin.id}#queryTabPersistence`,
      name: 'Experimental: Query Tab Persistence',
      description:
        'Persist query editor tabs to localStorage across sessions. ' +
        'Experimental: stored queries may be lost during version upgrades.',
      schema: z.boolean(),
      defaultValue: false,
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    const persistenceSetting = QueryPagePlugin.queryTabPersistenceSetting;

    // Debounced save to avoid writing on every keypress
    const debouncedSave = debounce(() => {
      saveTabsToStorage(persistenceSetting, editorTabs, activeTabId);
    }, 1000);

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

    // Try to restore tabs from localStorage if persistence is enabled
    const persistedState = loadTabsFromStorage(persistenceSetting);
    if (persistedState) {
      for (const tab of persistedState.tabs) {
        editorTabs.push({
          id: tab.id,
          editorText: tab.editorText,
          title: tab.title,
          queryResult: undefined,
          isLoading: false,
        });
      }
    } else {
      editorTabs.push(createNewTab());
    }

    let activeTabId =
      persistedState &&
      editorTabs.some((t) => t.id === persistedState.activeTabId)
        ? persistedState.activeTabId
        : editorTabs[0].id;

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
        debouncedSave();
      }
    }

    function onTabChange(tabId: string) {
      activeTabId = tabId;
      debouncedSave();
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

      debouncedSave();
    }

    function onTabAdd(
      tabName?: string,
      initialQuery?: string,
      autoExecute?: boolean,
    ) {
      const newTab = createNewTab(tabName, initialQuery);
      editorTabs.push(newTab);
      activeTabId = newTab.id;
      debouncedSave();

      if (autoExecute) {
        onExecute(newTab.id, initialQuery ?? '');
      }
    }

    function onTabRename(tabId: string, newName: string) {
      const tab = editorTabs.find((t) => t.id === tabId);
      if (tab) {
        tab.title = newName;
        debouncedSave();
      }
    }

    function onTabReorder(
      draggedTabId: string,
      beforeTabId: string | undefined,
    ) {
      const draggedIndex = editorTabs.findIndex((t) => t.id === draggedTabId);
      if (draggedIndex === -1) return;

      // Remove the dragged tab
      const [draggedTab] = editorTabs.splice(draggedIndex, 1);

      // Find where to insert it
      if (beforeTabId === undefined) {
        // Insert at the end
        editorTabs.push(draggedTab);
      } else {
        const beforeIndex = editorTabs.findIndex((t) => t.id === beforeTabId);
        if (beforeIndex === -1) {
          // beforeTabId not found, insert at end
          editorTabs.push(draggedTab);
        } else {
          editorTabs.splice(beforeIndex, 0, draggedTab);
        }
      }

      debouncedSave();
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
          onTabReorder,
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
