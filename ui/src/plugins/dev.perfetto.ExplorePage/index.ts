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
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {ExplorePage, ExplorePageState} from './explore_page';
import {nodeRegistry} from './query_builder/node_registry';
import {QueryNodeState} from './query_node';
import {deserializeState, serializeState} from './json_handler';
import {recentGraphsStorage} from './recent_graphs';
import {resetAnalyzeNodeSummarizer} from './query_builder/query_builder_utils';

const STORE_VERSION = 1;

interface ExplorePagePersistedState {
  version: number;
  graphJson?: string;
}

function isValidPersistedState(
  init: unknown,
): init is ExplorePagePersistedState {
  return (
    typeof init === 'object' &&
    init !== null &&
    'version' in init &&
    (init as {version: unknown}).version === STORE_VERSION
  );
}

/**
 * Loads the Explore Page state from recent graphs storage.
 * Returns undefined if no state is found or if deserialization fails.
 */
function loadStateFromRecentGraphs(trace: Trace): ExplorePageState | undefined {
  try {
    const json = recentGraphsStorage.getCurrentJson();
    if (!json) {
      return undefined;
    }

    const sqlModulesPlugin = trace.plugins.getPlugin(SqlModulesPlugin);
    const sqlModules = sqlModulesPlugin.getSqlModules();
    if (!sqlModules) {
      // SQL modules not yet initialized - return undefined to retry later
      return undefined;
    }

    return deserializeState(json, trace, sqlModules);
  } catch (error) {
    console.debug(
      'Failed to load Explore Page state from recent graphs:',
      error,
    );
    // Clear corrupted data to prevent repeated failures
    recentGraphsStorage.clear();
    return undefined;
  }
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.ExplorePage';
  static readonly dependencies = [SqlModulesPlugin];

  // The following allows us to have persistent
  // state/charts for the lifecycle of a single
  // trace.
  private state: ExplorePageState = {
    rootNodes: [],
    selectedNodes: new Set(),
    nodeLayouts: new Map(),
    labels: [],
  };

  // Track whether we've successfully loaded state from local storage
  private hasAttemptedStateLoad = false;

  // Track whether we've auto-initialized base JSON in this session
  // This prevents reloading base JSON when clearing all nodes
  private hasAutoInitialized = false;

  // Store for persisting state in permalinks
  private permalinkStore?: Store<ExplorePagePersistedState>;

  onStateUpdate = (
    update:
      | ExplorePageState
      | ((current: ExplorePageState) => ExplorePageState),
  ) => {
    if (typeof update === 'function') {
      this.state = update(this.state);
    } else {
      this.state = update;
    }

    // Save current state to recent graphs (updates the first entry)
    recentGraphsStorage.saveCurrentState(this.state);

    // Save to permalink store for sharing (clear if graph is empty)
    if (this.permalinkStore) {
      const graphJson =
        this.state.rootNodes.length > 0
          ? serializeState(this.state)
          : undefined;
      this.permalinkStore.edit((draft) => {
        draft.graphJson = graphJson;
      });
    }

    m.redraw();
  };

  // Mount the permalink store lazily on first page access.
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

  // Try to load state from permalink store or recent graphs.
  // Called lazily when the page renders.
  private tryLoadState(trace: Trace): void {
    if (this.hasAttemptedStateLoad) return;

    // Mount permalink store on first access
    this.mountPermalinkStore(trace);

    const sqlModulesPlugin = trace.plugins.getPlugin(SqlModulesPlugin);
    const sqlModules = sqlModulesPlugin.getSqlModules();
    if (!sqlModules) {
      // SQL modules not ready yet, we'll retry on next render
      return;
    }

    // SQL modules are ready, mark load as attempted regardless of outcome
    this.hasAttemptedStateLoad = true;

    // First, check permalink store (for graphs restored from permalinks)
    const permalinkJson = this.permalinkStore?.state.graphJson;
    if (permalinkJson) {
      try {
        const permalinkState = deserializeState(
          permalinkJson,
          trace,
          sqlModules,
        );
        this.state = permalinkState;
        this.hasAutoInitialized = true;
        return;
      } catch (e) {
        const msg = e instanceof Error ? e.message : String(e);
        console.warn('Failed to load Explore Page state from permalink:', msg);
        // Fall through to try recent graphs
      }
    }

    // Fall back to recent graphs (local storage)
    const savedState = loadStateFromRecentGraphs(trace);
    if (savedState !== undefined) {
      // Load saved state from recent graphs (preserves work across page refreshes)
      this.state = savedState;
      // Only mark as auto-initialized if the saved state has nodes
      // This allows base JSON to load after a reload when state is empty,
      // but prevents it from loading after manual "Clear all nodes" in the same session
      if (savedState.rootNodes.length > 0) {
        this.hasAutoInitialized = true;
      }
    }
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    // Reset module-level state from previous traces to prevent stale IDs.
    resetAnalyzeNodeSummarizer();

    trace.pages.registerPage({
      route: '/explore',
      render: () => {
        // Ensure SQL modules initialization is triggered (no-op if already started)
        trace.plugins.getPlugin(SqlModulesPlugin).ensureInitialized();

        // Try to load saved state lazily (waits for SQL modules to be ready)
        this.tryLoadState(trace);

        return m(ExplorePage, {
          trace,
          state: this.state,
          sqlModulesPlugin: trace.plugins.getPlugin(SqlModulesPlugin),
          onStateUpdate: this.onStateUpdate,
          hasAutoInitialized: this.hasAutoInitialized,
          setHasAutoInitialized: (value: boolean) => {
            this.hasAutoInitialized = value;
          },
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

        // Add node to state and select it
        this.onStateUpdate((currentState) => ({
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
