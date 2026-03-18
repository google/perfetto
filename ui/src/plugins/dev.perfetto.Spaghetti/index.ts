// Copyright (C) 2025 The Android Open Source Project
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
import {QueryBuilderPage} from './query_builder_page';
import {NodeQueryBuilderStore} from './node_types';
import {ColumnDef, getOutputColumnsForNode} from './graph_utils';

/**
 * Callback interface for external access to the query builder's state.
 * Registered by QueryBuilderPage when it mounts.
 */
export interface QueryBuilderDelegate {
  getStore(): NodeQueryBuilderStore;
  setStore(store: NodeQueryBuilderStore): void;
  serializeStore(): string;
  deserializeAndSetStore(json: string): void;
  selectNode(nodeId: string): void;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Spaghetti';
  static readonly dependencies = [SqlModulesPlugin];

  // Delegate registered by the QueryBuilderPage component.
  private delegate?: QueryBuilderDelegate;

  /**
   * Register a delegate that provides access to the query builder state.
   * Called by QueryBuilderPage on mount.
   */
  registerDelegate(delegate: QueryBuilderDelegate): void {
    this.delegate = delegate;
  }

  /**
   * Unregister the delegate. Called by QueryBuilderPage on unmount.
   */
  unregisterDelegate(): void {
    this.delegate = undefined;
  }

  /**
   * Get the current graph as a serialized JSON string.
   * Returns undefined if the query builder page is not mounted.
   */
  getGraphJson(): string | undefined {
    return this.delegate?.serializeStore();
  }

  /**
   * Replace the current graph from a JSON string.
   * Throws if the query builder page is not mounted.
   */
  loadGraphJson(json: string): void {
    if (!this.delegate) {
      throw new Error(
        'Query builder page is not open. Navigate to #!/querybuilder first.',
      );
    }
    this.delegate.deserializeAndSetStore(json);
  }

  /**
   * Select a node by ID so its results are shown.
   */
  selectNode(nodeId: string): void {
    this.delegate?.selectNode(nodeId);
  }

  /**
   * Get the output columns for a node by ID.
   * Returns undefined if the page is not mounted or node not found.
   */
  getNodeOutputColumns(nodeId: string, trace: Trace): ColumnDef[] | undefined {
    if (!this.delegate) return undefined;
    const store = this.delegate.getStore();
    const sqlModulesPlugin = trace.plugins.getPlugin(SqlModulesPlugin);
    const sqlModules = sqlModulesPlugin.getSqlModules();
    return getOutputColumnsForNode(
      store.nodes,
      store.connections,
      nodeId,
      sqlModules,
    );
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    const sqlModulesPlugin = trace.plugins.getPlugin(SqlModulesPlugin);

    trace.pages.registerPage({
      route: '/spaghetti',
      render: () => {
        sqlModulesPlugin.ensureInitialized();
        return m(QueryBuilderPage, {
          trace,
          sqlModules: sqlModulesPlugin.getSqlModules(),
          plugin: this,
        });
      },
    });

    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Spaghetti',
      href: '#!/spaghetti',
      icon: 'cable',
      sortOrder: 22,
    });
  }
}
