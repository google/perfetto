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
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {Store} from '../../base/store';
import {shortUuid} from '../../base/uuid';
import {getErrorMessage} from '../../base/errors';
import {debounce} from '../../base/rate_limiters';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {ExplorePage, ExplorePageState, ExploreTab} from './explore_page';
import {nodeRegistry} from './query_builder/node_registry';
import {QueryNodeState} from './query_node';
import {deserializeState, serializeState} from './json_handler';
import {recentGraphsStorage} from './recent_graphs';
import {
  exploreTabsStorage,
  createNewTabName,
  createEmptyState,
} from './explore_tabs_storage';
import type {PersistedExploreTabData} from './explore_tabs_storage';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';

// --- Permalink persistence ---

const STORE_VERSION = 2;

interface ExplorePagePersistedState {
  version: number;
  // Multi-tab format (version 2+)
  tabs?: PersistedExploreTabData[];
  activeTabId?: string;
  // Old single-graph format (version 1) - kept for backward compat
  graphJson?: string;
}

function isValidPersistedState(
  init: unknown,
): init is ExplorePagePersistedState {
  if (typeof init !== 'object' || init === null || !('version' in init)) {
    return false;
  }
  const version = (init as {version: unknown}).version;
  // Accept both v1 (old single-graph) and v2 (multi-tab)
  return version === 1 || version === STORE_VERSION;
}

// --- Plugin ---

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.ExplorePage';
  static readonly dependencies = [SqlModulesPlugin];

  // Multi-tab state
  private tabs: ExploreTab[] = [];
  private activeTabId = '';

  // Track whether we've successfully loaded state from local storage
  private hasAttemptedStateLoad = false;

  // Store for persisting state in permalinks
  private permalinkStore?: Store<ExplorePagePersistedState>;

  // Debounced saves to avoid expensive serialization on every state change
  private debouncedSave = debounce(() => {
    exploreTabsStorage.save(this.tabs, this.activeTabId);
  }, 1000);

  private debouncedPermalinkSave = debounce(() => {
    this.saveToPermalinkStore();
  }, 1000);

  // Flush pending saves on page unload to avoid data loss
  private readonly onBeforeUnload = () => {
    try {
      exploreTabsStorage.save(this.tabs, this.activeTabId);
      this.saveToPermalinkStore();
    } catch (e) {
      console.warn('Failed to flush explore tabs on unload:', e);
    }
  };

  // --- Tab helpers ---

  private createNewTab(title?: string): ExploreTab {
    return {
      id: shortUuid(),
      title: title ?? createNewTabName(this.tabs),
      state: createEmptyState(),
    };
  }

  private getActiveTab(): ExploreTab | undefined {
    return this.tabs.find((t) => t.id === this.activeTabId);
  }

  private ensureAtLeastOneTab(): void {
    if (this.tabs.length === 0) {
      const tab = this.createNewTab();
      this.tabs.push(tab);
      this.activeTabId = tab.id;
    }
  }

  // --- Tab CRUD ---

  private handleTabAdd = (): void => {
    const newTab = this.createNewTab();
    this.tabs.push(newTab);
    this.activeTabId = newTab.id;
    this.debouncedSave();
    m.redraw();
  };

  private handleTabClose = (tabId: string): void => {
    const index = this.tabs.findIndex((t) => t.id === tabId);
    if (index === -1) return;

    // Don't close the last tab
    if (this.tabs.length === 1) return;

    this.tabs.splice(index, 1);

    // If we closed the active tab, switch to an adjacent one
    if (this.activeTabId === tabId) {
      const newIndex = Math.min(index, this.tabs.length - 1);
      this.activeTabId = this.tabs[newIndex].id;
    }

    this.debouncedSave();
    m.redraw();
  };

  private handleTabChange = (tabId: string): void => {
    this.activeTabId = tabId;
    this.debouncedSave();
    m.redraw();
  };

  private handleTabRename = (tabId: string, newName: string): void => {
    const tab = this.tabs.find((t) => t.id === tabId);
    if (tab) {
      tab.title = newName;
      this.debouncedSave();
      m.redraw();
    }
  };

  private handleTabReorder = (
    draggedTabId: string,
    beforeTabId: string | undefined,
  ): void => {
    const draggedIndex = this.tabs.findIndex((t) => t.id === draggedTabId);
    if (draggedIndex === -1) return;

    const [draggedTab] = this.tabs.splice(draggedIndex, 1);

    if (beforeTabId === undefined) {
      this.tabs.push(draggedTab);
    } else {
      const beforeIndex = this.tabs.findIndex((t) => t.id === beforeTabId);
      if (beforeIndex === -1) {
        this.tabs.push(draggedTab);
      } else {
        this.tabs.splice(beforeIndex, 0, draggedTab);
      }
    }

    this.debouncedSave();
  };

  // --- Per-tab state update ---

  private makeOnStateUpdate(tabId: string) {
    return (
      update:
        | ExplorePageState
        | ((current: ExplorePageState) => ExplorePageState),
    ) => {
      const tab = this.tabs.find((t) => t.id === tabId);
      if (!tab) return;

      if (typeof update === 'function') {
        tab.state = update(tab.state);
      } else {
        tab.state = update;
      }

      // Save active tab's state to recent graphs (updates the working slot)
      if (tabId === this.activeTabId) {
        recentGraphsStorage.saveCurrentState(tab.state);
      }

      // Save all tabs to permalink store (debounced)
      this.debouncedPermalinkSave();

      // Save all tabs to localStorage (debounced)
      this.debouncedSave();

      m.redraw();
    };
  }

  // --- Permalink store ---

  private mountPermalinkStore(trace: Trace): void {
    if (this.permalinkStore) return;

    this.permalinkStore = trace.mountStore<ExplorePagePersistedState>(
      'dev.perfetto.ExplorePage',
      (init: unknown) => {
        if (isValidPersistedState(init)) {
          return init;
        }
        return {version: STORE_VERSION};
      },
    );
  }

  private saveToPermalinkStore(): void {
    if (!this.permalinkStore) return;

    const tabsData: PersistedExploreTabData[] = this.tabs
      .filter((tab) => tab.state.rootNodes.length > 0)
      .map((tab) => ({
        id: tab.id,
        title: tab.title,
        graphJson: serializeState(tab.state),
      }));

    this.permalinkStore.edit((draft) => {
      draft.version = STORE_VERSION;
      draft.tabs = tabsData.length > 0 ? tabsData : undefined;
      draft.activeTabId = this.activeTabId;
      // Clear deprecated single-graph field
      draft.graphJson = undefined;
    });
  }

  // --- State loading ---

  /** Hydrate tabs from persisted tab data, returning the list of loaded tabs. */
  private hydrateTabs(
    tabsData: ReadonlyArray<{
      id: string;
      title: string;
      graphJson?: string;
    }>,
    trace: Trace,
    sqlModules: SqlModules,
  ): ExploreTab[] {
    return tabsData.map((tabData) => {
      const state =
        tabData.graphJson !== undefined
          ? deserializeState(tabData.graphJson, trace, sqlModules)
          : createEmptyState();
      return {
        id: tabData.id,
        title: tabData.title,
        state,
      };
    });
  }

  private tryLoadState(trace: Trace): void {
    if (this.hasAttemptedStateLoad) return;

    this.mountPermalinkStore(trace);

    const sqlModulesPlugin = trace.plugins.getPlugin(SqlModulesPlugin);
    const sqlModules = sqlModulesPlugin.getSqlModules();
    if (!sqlModules) {
      // SQL modules not ready yet, we'll retry on next render
      return;
    }

    // SQL modules are ready, mark load as attempted regardless of outcome
    this.hasAttemptedStateLoad = true;

    // Priority 1: Check permalink store
    const permalinkState = this.permalinkStore?.state;
    if (permalinkState) {
      // Try multi-tab format first (version 2+)
      if (permalinkState.tabs !== undefined && permalinkState.tabs.length > 0) {
        try {
          this.tabs = this.hydrateTabs(permalinkState.tabs, trace, sqlModules);
          this.activeTabId =
            permalinkState.activeTabId !== undefined &&
            this.tabs.some((t) => t.id === permalinkState.activeTabId)
              ? permalinkState.activeTabId
              : this.tabs[0].id;
          return;
        } catch (e) {
          const msg = getErrorMessage(e);
          console.warn('Failed to load Explore Page tabs from permalink:', msg);
          this.tabs = [];
          // Fall through to try other sources
        }
      }

      // Try old single-graph format (version 1 backward compat)
      if (permalinkState.graphJson !== undefined) {
        try {
          const state = deserializeState(
            permalinkState.graphJson,
            trace,
            sqlModules,
          );
          const tab = this.createNewTab();
          tab.state = state;
          this.tabs.push(tab);
          this.activeTabId = tab.id;
          return;
        } catch (e) {
          const msg = getErrorMessage(e);
          console.warn(
            'Failed to load Explore Page state from permalink:',
            msg,
          );
          // Fall through to try other sources
        }
      }
    }

    // Priority 2: Check new localStorage tabs key
    const persistedTabs = exploreTabsStorage.load();
    if (persistedTabs !== undefined) {
      try {
        this.tabs = this.hydrateTabs(persistedTabs.tabs, trace, sqlModules);
        this.activeTabId = this.tabs.some(
          (t) => t.id === persistedTabs.activeTabId,
        )
          ? persistedTabs.activeTabId
          : this.tabs[0].id;
        return;
      } catch (e) {
        console.debug('Failed to load Explore Page tabs from localStorage:', e);
        this.tabs = [];
        // Fall through to try recent graphs
      }
    }

    // Priority 3: Backward compat - try old recentGraphsStorage
    try {
      const json = recentGraphsStorage.getCurrentJson();
      if (json) {
        const state = deserializeState(json, trace, sqlModules);
        const tab = this.createNewTab();
        tab.state = state;
        this.tabs.push(tab);
        this.activeTabId = tab.id;
        return;
      }
    } catch (e) {
      console.debug('Failed to load Explore Page state from recent graphs:', e);
      recentGraphsStorage.clear();
    }

    // Priority 4: Create one empty default tab
    this.ensureAtLeastOneTab();
  }

  // --- Plugin lifecycle ---

  async onTraceLoad(trace: Trace): Promise<void> {
    // Flush pending localStorage saves on page unload
    window.addEventListener('beforeunload', this.onBeforeUnload);
    trace.trash.defer(() => {
      window.removeEventListener('beforeunload', this.onBeforeUnload);
    });

    trace.pages.registerPage({
      route: '/explore',
      render: () => {
        // Try to load saved state lazily (waits for SQL modules to be ready).
        this.tryLoadState(trace);

        const activeTab = this.getActiveTab();
        if (!activeTab) {
          return m('.pf-explore-page', 'Loading...');
        }

        return m(ExplorePage, {
          trace,
          tabs: this.tabs,
          activeTabId: this.activeTabId,
          state: activeTab.state,
          sqlModulesPlugin: trace.plugins.getPlugin(SqlModulesPlugin),
          onStateUpdate: this.makeOnStateUpdate(this.activeTabId),
          makeOnStateUpdate: (tabId: string) => this.makeOnStateUpdate(tabId),
          onTabAdd: this.handleTabAdd,
          onTabClose: this.handleTabClose,
          onTabChange: this.handleTabChange,
          onTabRename: this.handleTabRename,
          onTabReorder: this.handleTabReorder,
        });
      },
    });
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 20,
      text: 'Data Explorer',
      href: '#!/explore',
      icon: 'data_exploration',
    });

    // Register "Move selection to Explore Page" command
    trace.commands.registerCommand({
      id: 'dev.perfetto.ExplorePage.MoveSelectionToExplorePage',
      name: 'Move selection to Explore Page',
      callback: () => {
        const timeSpan = trace.selection.getTimeSpanOfSelection();
        if (!timeSpan) {
          // No valid time selection - inform user
          console.warn(
            'No time selection found. Please select a time range on the timeline first.',
          );
          return;
        }

        // Capture the time range values before clearing selection
        const start = timeSpan.start;
        const end = timeSpan.end;

        // Clear the timeline selection FIRST to avoid UI artifacts
        trace.selection.clearSelection();

        // Get the TimeRange node descriptor
        const descriptor = nodeRegistry.get('timerange');
        if (!descriptor) {
          console.error('TimeRange node not found in registry');
          return;
        }

        // Create the TimeRange node with captured values
        const newNode = descriptor.factory({
          trace,
          start,
          end,
        } as unknown as QueryNodeState);

        // Ensure we have an active tab
        this.ensureAtLeastOneTab();

        // Add node to active tab's state
        const onStateUpdate = this.makeOnStateUpdate(this.activeTabId);
        onStateUpdate((currentState) => ({
          ...currentState,
          rootNodes: [...currentState.rootNodes, newNode],
          selectedNodes: new Set([newNode.nodeId]),
        }));

        // Navigate to Explore Page
        trace.navigate('#!/explore');
      },
    });
  }
}
