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
import {Button} from '../../widgets/button';
import {EmptyState} from '../../widgets/empty_state';
import {SplitPanel} from '../../widgets/split_panel';
import {Spinner} from '../../widgets/spinner';
import {Tabs, TabsTab} from '../../widgets/tabs';
import {QueryHistoryComponent} from '../query/query_history';
import {QueryRunner} from '../query/query_runner';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {sqlTablesLoader} from '../query/sql_tables';
import {TableList} from '../query/table_list';
import {EditorTabView} from './editor_tab_view';
import {QueryTabsState} from './query_tabs_state';

interface QueryPageAttrs {
  useBigtraceBackend?: boolean;
  initialQuery?: string;
}

// Lets the globally-registered keyboard command reach into the active
// QueryPage instance. Same pattern as sidebarToggleFn in index.ts.
export let queryRightSidebarToggleFn: (() => void) | undefined;

// Module-level singletons. Survive route navigations
// (/query → /settings → /query) so in-flight sync queries don't get
// orphaned with the destroyed QueryPage instance, and so tab state
// (`isLoading`, `clientStartTime`, `queryResult`, `dataSource`,
// `BigtraceAsyncDataSource` polling) carries over. Async query state
// already had a localStorage fallback via queryUuid, but sync queries
// are UUID-less from the SPA's view and have no recovery path —
// they'd vanish on every page switch.
const sharedTabsState = new QueryTabsState();
let sharedHistoryRefreshSignal = 0;
const sharedRunner = new QueryRunner({
  onHistoryChanged: () => {
    sharedHistoryRefreshSignal++;
  },
  markDirty: () => sharedTabsState.markDirty(),
});

export class QueryPage implements m.ClassComponent<QueryPageAttrs> {
  private useBigtraceBackend = false;
  private sidebarVisible = true;

  oninit({attrs}: m.Vnode<QueryPageAttrs>) {
    this.useBigtraceBackend = attrs.useBigtraceBackend || false;
    queryRightSidebarToggleFn = () => {
      this.sidebarVisible = !this.sidebarVisible;
      m.redraw();
    };
    if (attrs.initialQuery) {
      const activeTab = sharedTabsState.getActiveTab();
      if (activeTab && activeTab.editorText.trim() === '') {
        // Reuse the empty active tab and derive its title manually
        // (addNewTab's title path doesn't fire on this branch).
        activeTab.editorText = attrs.initialQuery;
        sharedTabsState.maybeAutoNameTab(activeTab.id, attrs.initialQuery);
      } else {
        // addNewTab already derives the title from initialQuery (B62).
        sharedTabsState.addNewTab(undefined, attrs.initialQuery);
      }
      sharedTabsState.markDirty();
    }
    if (this.useBigtraceBackend) {
      bigTraceSettingsStorage.loadSettings();
    }
    sqlTablesLoader.load();
  }

  view() {
    // Build editor tabs for the Tabs widget.
    const editorTabs: TabsTab[] = sharedTabsState.tabs.map((tab) => ({
      key: tab.id,
      title: tab.title,
      // Spinner on tabs with a query in flight, so tab-switching
      // doesn't make the running query "disappear".
      leftIcon: tab.isLoading ? 'progress_activity' : 'code',
      closeButton: sharedTabsState.tabs.length > 1,
      content: m(EditorTabView, {
        tab,
        tabsState: sharedTabsState,
        runner: sharedRunner,
        useBigtraceBackend: this.useBigtraceBackend,
      }),
    }));

    const leftPanel = m(Tabs, {
      className: 'pf-query-page__editor-tabs',
      tabs: editorTabs,
      activeTabKey: sharedTabsState.activeTabId,
      reorderable: true,
      onTabChange: (key) => {
        sharedTabsState.activeTabId = key;
        sharedTabsState.markDirty();
      },
      onTabRename: (key, newTitle) => sharedTabsState.renameTab(key, newTitle),
      onTabClose: (key) => {
        // closeTab is a no-op when only one tab remains; bail before
        // the confirm so middle-click doesn't dead-end.
        if (sharedTabsState.tabs.length <= 1) return;
        // Confirm before closing a tab with an in-flight sync query
        // (Persistent queries keep running on the backend).
        const tab = sharedTabsState.tabs.find((t) => t.id === key);
        if (
          tab?.isLoading &&
          !window.confirm('A query is still running in this tab. Close anyway?')
        ) {
          return;
        }
        sharedTabsState.closeTab(key);
      },
      onTabReorder: (draggedKey, beforeKey) =>
        sharedTabsState.reorderTab(draggedKey, beforeKey),
      newTabContent: [
        m(Button, {
          icon: 'add',
          className: 'pf-tabs__new-tab-btn',
          onclick: () => sharedTabsState.addNewTab(),
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
          // No leftIcon — the ~20px is better spent on the label at
          // narrow viewports.
          title: 'History',
          content: m(QueryHistoryComponent, {
            className: 'pf-query-page__history',
            refreshSignal: sharedHistoryRefreshSignal,
            openQuery: async (
              query: string,
              uuid: string,
              materialize: boolean,
              forceNew?: boolean,
              limit?: number,
              startTime?: number,
            ) => {
              const tab = sharedTabsState.addNewTab(
                undefined,
                query,
                limit,
                uuid,
                materialize,
                forceNew,
              );
              sharedTabsState.activeTabId = tab.id;
              sharedTabsState.markDirty();
              if (startTime !== undefined && tab.execution) {
                tab.execution.startTime = startTime;
              }
              await sharedRunner.resumeFromHistory(tab, query);
            },
          }),
        },
        {
          key: 'tables',
          // Hide the count until the loader settles so we don't
          // flash "(0)" on mount.
          title:
            sqlTablesLoader.modules && !sqlTablesLoader.isLoading
              ? `Stdlib Schemas (${sqlTablesLoader.modules.listTables().length})`
              : 'Stdlib Schemas',
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
        // Floor for the History meta-band layout; dismiss the
        // sidebar entirely (Ctrl+Shift+B) for narrower screens.
        minSize: 280,
        firstPanel: leftPanel,
        secondPanel: sidebarPanel,
      }),
    );
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
        sharedTabsState.addNewTab(tableName, query);
      },
    });
  }
}
