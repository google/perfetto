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
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {ExplorePage, ExplorePageState} from './explore_page';
import {nodeRegistry} from './query_builder/node_registry';
import {QueryNodeState} from './query_node';
import {serializeState, deserializeState} from './json_handler';

const LOCAL_STORAGE_KEY = 'perfetto.explorePage.lastState';

/**
 * Saves the Explore Page state to local storage.
 */
function saveStateToLocalStorage(state: ExplorePageState): void {
  try {
    const json = serializeState(state);
    localStorage.setItem(LOCAL_STORAGE_KEY, json);
  } catch (error) {
    console.warn('Failed to save Explore Page state to local storage:', error);
  }
}

/**
 * Loads the Explore Page state from local storage.
 * Returns undefined if no state is found or if deserialization fails.
 */
function loadStateFromLocalStorage(trace: Trace): ExplorePageState | undefined {
  try {
    const json = localStorage.getItem(LOCAL_STORAGE_KEY);
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
      'Failed to load Explore Page state from local storage:',
      error,
    );
    // Clear invalid state
    localStorage.removeItem(LOCAL_STORAGE_KEY);
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
    nodeLayouts: new Map(),
  };

  // Track whether we've successfully loaded state from local storage
  private hasAttemptedStateLoad = false;

  // Track whether we've auto-initialized base JSON in this session
  // This prevents reloading base JSON when clearing all nodes
  private hasAutoInitialized = false;

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

    saveStateToLocalStorage(this.state);

    m.redraw();
  };

  // Try to load state from local storage. Called lazily when the page renders.
  private tryLoadState(trace: Trace): void {
    if (this.hasAttemptedStateLoad) return;

    const savedState = loadStateFromLocalStorage(trace);
    if (savedState !== undefined) {
      // Load saved state from localStorage (preserves work across page refreshes)
      this.state = savedState;
      this.hasAttemptedStateLoad = true;
    } else if (
      trace.plugins.getPlugin(SqlModulesPlugin).getSqlModules() !== undefined
    ) {
      // SQL modules are available but no state was loaded - mark as attempted
      // to avoid retrying on every render
      this.hasAttemptedStateLoad = true;
    }
    // If SQL modules aren't ready yet, we'll retry on next render
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.pages.registerPage({
      route: '/explore',
      render: () => {
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
      sortOrder: 21,
      text: 'Explore',
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
          selectedNode: newNode,
        }));

        // Navigate to Explore Page
        trace.navigate('#!/explore');
      },
    });
  }
}
