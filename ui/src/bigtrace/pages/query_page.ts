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
import {showModal} from '../../widgets/modal';
import {EditorTabView} from './editor_tab_view';
import {QueryTabsState} from './query_tabs_state';
import {queryState} from '../query/query_state';

interface QueryPageAttrs {
  useBigtraceBackend?: boolean;
}

// Lets the globally-registered keyboard command reach into the active
// QueryPage instance. Same pattern as sidebarToggleFn in index.ts.
export let queryRightSidebarToggleFn: (() => void) | undefined;

export class QueryPage implements m.ClassComponent<QueryPageAttrs> {
  private useBigtraceBackend = false;
  private sidebarVisible = true;
  private readonly tabsState = new QueryTabsState();
  private historyRefreshSignal = 0;
  private readonly runner = new QueryRunner({
    onHistoryChanged: () => {
      this.historyRefreshSignal++;
    },
    markDirty: () => this.tabsState.markDirty(),
  });

  oninit({attrs}: m.Vnode<QueryPageAttrs>) {
    this.useBigtraceBackend = attrs.useBigtraceBackend || false;
    queryRightSidebarToggleFn = () => {
      this.sidebarVisible = !this.sidebarVisible;
      m.redraw();
    };
    if (this.useBigtraceBackend) {
      bigTraceSettingsStorage.loadSettings();
    }
    sqlTablesLoader.load();
  }

  view() {
    // Process initialQuery set by home-page example buttons.
    // Read-and-clear: each value is consumed exactly once.
    const initialQuery = queryState.initialQuery;
    if (initialQuery !== undefined) {
      queryState.initialQuery = undefined;
      const activeTab = this.tabsState.getActiveTab();
      if (activeTab && activeTab.editorText.trim() === '') {
        activeTab.editorText = initialQuery;
        this.tabsState.maybeAutoNameTab(activeTab.id, initialQuery);
      } else {
        this.tabsState.addNewTab(undefined, initialQuery);
      }
      this.tabsState.markDirty();
    }

    // Build editor tabs for the Tabs widget.
    const editorTabs: TabsTab[] = this.tabsState.tabs.map((tab) => ({
      key: tab.id,
      title: tab.title,
      // Spinner on tabs with a query in flight, so tab-switching
      // doesn't make the running query "disappear".
      leftIcon: tab.isLoading ? 'progress_activity' : 'code',
      closeButton: this.tabsState.tabs.length > 1,
      content: m(EditorTabView, {
        tab,
        tabsState: this.tabsState,
        runner: this.runner,
        useBigtraceBackend: this.useBigtraceBackend,
      }),
    }));

    const leftPanel = m(Tabs, {
      className: 'pf-query-page__editor-tabs',
      tabs: editorTabs,
      activeTabKey: this.tabsState.activeTabId,
      reorderable: true,
      onTabChange: (key) => {
        this.tabsState.activeTabId = key;
        this.tabsState.markDirty();
      },
      onTabRename: (key, newTitle) => this.tabsState.renameTab(key, newTitle),
      onTabClose: async (key) => {
        // closeTab is a no-op when only one tab remains; bail before
        // the confirm so middle-click doesn't dead-end.
        if (this.tabsState.tabs.length <= 1) return;
        // Confirm only for ephemeral queries — closing loses the results.
        // Persistent queries keep running on the backend (reopen from History).
        const tab = this.tabsState.tabs.find((t) => t.id === key);
        if (tab?.isLoading && !tab.materialize) {
          let confirmed = false;
          await showModal({
            title: 'Close tab?',
            content: m(
              'div',
              'A query is still running. Closing this tab will lose the results.',
            ),
            buttons: [
              {text: 'Keep open'},
              {
                text: 'Close',
                primary: true,
                action: () => {
                  confirmed = true;
                },
              },
            ],
          });
          if (!confirmed) return;
        }
        this.tabsState.closeTab(key);
        m.redraw();
      },
      onTabReorder: (draggedKey, beforeKey) =>
        this.tabsState.reorderTab(draggedKey, beforeKey),
      newTabContent: [
        m(Button, {
          icon: 'add',
          className: 'pf-tabs__new-tab-btn',
          onclick: () => this.tabsState.addNewTab(),
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
            refreshSignal: this.historyRefreshSignal,
            openQuery: async (
              query: string,
              uuid: string,
              materialize: boolean,
              forceNew?: boolean,
              limit?: number,
              startTime?: number,
            ) => {
              const tab = this.tabsState.addNewTab(
                undefined,
                query,
                limit,
                uuid,
                materialize,
                forceNew,
              );
              this.tabsState.activeTabId = tab.id;
              this.tabsState.markDirty();
              if (startTime !== undefined && tab.execution) {
                tab.execution.startTime = startTime;
              }
              await this.runner.resumeFromHistory(tab, query);
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
        this.tabsState.addNewTab(tableName, query);
      },
    });
  }
}
