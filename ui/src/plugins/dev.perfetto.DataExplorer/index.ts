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
import QueryPagePlugin from '../dev.perfetto.QueryPage';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {DataExplorer, DataExplorerState, DataExplorerTab} from './data_explorer';
import {nodeRegistry} from './query_builder/node_registry';
import {QueryNodeState} from './query_node';
import {deserializeState, serializeState} from './json_handler';
import {recentGraphsStorage} from './recent_graphs';
import {
  dataExplorerTabsStorage,
  createNewTabName,
  createEmptyState,
} from './data_explorer_tabs_storage';
import type {PersistedDataExplorerTabData} from './data_explorer_tabs_storage';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';

// --- Permalink persistence ---

const STORE_VERSION = 2;

interface DataExplorerPersistedState {
  version: number;
  // Multi-tab format (version 2+)
  tabs?: PersistedDataExplorerTabData[];
  activeTabId?: string;
  // Old single-graph format (version 1) - kept for backward compat
  graphJson?: string;
}

function isValidPersistedState(
  init: unknown,
): init is DataExplorerPersistedState {
  if (typeof init !== 'object' || init === null || !('version' in init)) {
    return false;
  }
  const version = (init as {version: unknown}).version;
  // Accept both v1 (old single-graph) and v2 (multi-tab)
  return version === 1 || version === STORE_VERSION;
}

// --- Plugin ---

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.DataExplorer';
  static readonly dependencies = [QueryPagePlugin, SqlModulesPlugin];

  // Multi-tab state
  private tabs: DataExplorerTab[] = [];
  private activeTabId = '';

  // Track whether we've successfully loaded state from local storage
  private hasAttemptedStateLoad = false;

  // Store for persisting state in permalinks
  private permalinkStore?: Store<DataExplorerPersistedState>;

  // Debounced saves to avoid expensive serialization on every state change
  private debouncedSave = debounce(() => {
    dataExplorerTabsStorage.save(this.tabs, this.activeTabId);
  }, 1000);

  private debouncedPermalinkSave = debounce(() => {
    this.saveToPermalinkStore();
  }, 1000);

  // Flush pending saves on page unload to avoid data loss
  private readonly onBeforeUnload = () => {
    try {
      dataExplorerTabsStorage.save(this.tabs, this.activeTabId);
      this.saveToPermalinkStore();
    } catch (e) {
      console.warn('Failed to flush data explorer tabs on unload:', e);
    }
  };

  // --- Tab helpers ---

  private createNewTab(title?: string): DataExplorerTab {
    return {
      id: shortUuid(),
      title: title ?? createNewTabName(this.tabs),
      state: createEmptyState(),
    };
  }

  private getActiveTab(): DataExplorerTab | undefined {
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
    if (!tab) return;
    const trimmed = newName.trim();
    if (trimmed === '') return;
    const isDuplicate = this.tabs.some(
      (t) => t.id !== tabId && t.title === trimmed,
    );
    if (isDuplicate) return;
    tab.title = trimmed;
    this.debouncedSave();
    m.redraw();
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

  private handleTabAddWithState = (
    title: string,
    state: DataExplorerState,
    afterTabId: string,
  ): void => {
    const newTab: DataExplorerTab = {
      id: shortUuid(),
      title,
      state,
    };

    const afterIndex = this.tabs.findIndex((t) => t.id === afterTabId);
    if (afterIndex !== -1) {
      this.tabs.splice(afterIndex + 1, 0, newTab);
    } else {
      this.tabs.push(newTab);
    }
    this.activeTabId = newTab.id;

    this.debouncedSave();
    m.redraw();
  };

  // --- Per-tab state update ---

  private makeOnStateUpdate(tabId: string) {
    return (
      update:
        | DataExplorerState
        | ((current: DataExplorerState) => DataExplorerState),
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

    this.permalinkStore = trace.mountStore<DataExplorerPersistedState>(
      'dev.perfetto.DataExplorer',
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

    const tabsData: PersistedDataExplorerTabData[] = this.tabs
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
  ): DataExplorerTab[] {
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

    this.loadStateFromSources(trace, sqlModules);

    // Sync loaded state to the permalink store so that "Share trace" includes
    // the Data Explorer state even if the user hasn't modified anything.
    // Without this, state loaded from localStorage or recent graphs would
    // never be written to the permalink store, causing permalinks to lose
    // the Data Explorer state.
    this.saveToPermalinkStore();
  }

  private loadStateFromSources(trace: Trace, sqlModules: SqlModules): void {
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
          console.warn('Failed to load Data Explorer tabs from permalink:', msg);
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
            'Failed to load Data Explorer state from permalink:',
            msg,
          );
          // Fall through to try other sources
        }
      }
    }

    // Priority 2: Check new localStorage tabs key
    const persistedTabs = dataExplorerTabsStorage.load();
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
        console.debug('Failed to load Data Explorer tabs from localStorage:', e);
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
      console.debug('Failed to load Data Explorer state from recent graphs:', e);
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
        // Ensure SQL modules initialization is triggered (no-op if already
        // started). This kicks off the data availability checks that determine
        // which modules should be marked as "No data".
        trace.plugins.getPlugin(SqlModulesPlugin).ensureInitialized();

        // Try to load saved state lazily (waits for SQL modules to be ready).
        this.tryLoadState(trace);

        const activeTab = this.getActiveTab();
        if (!activeTab) {
          return m('.pf-data-explorer', 'Loading...');
        }

        return m(DataExplorer, {
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
          onTabAddWithState: this.handleTabAddWithState,
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

    // Register "Move selection to Data Explorer" command
    trace.commands.registerCommand({
      id: 'dev.perfetto.DataExplorer.MoveSelectionToDataExplorer',
      name: 'Move selection to Data Explorer',
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

        // Navigate to Data Explorer
        trace.navigate('#!/explore');
      },
    });
  }
}
