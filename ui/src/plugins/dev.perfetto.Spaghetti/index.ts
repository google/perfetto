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
import {QueryBuilderPage} from './views/query_builder_page';
import {
  ColumnDef,
  getManifest,
  getManifestInputs,
  isNodeValid,
  getOutputColumnsForNode,
} from './graph_utils';
import {NodeData, NodeQueryBuilderStore} from './graph_model';

/*
Note: This is an experiment representing some ideas for how a query builder
could be architected and how it could look and feel.

- **On-node configuration (no sidebar):** This puts the node config right where
  the graph is and avoids mousing around, while also making it easier to read
  the graph as all the information is right there. You see something you want to
  change, you click it, you change it.
- **Immutable (immer managed) graph state:** This allows for simple and
  efficient undo/redo, and change detection.
- **JS-side column name and type inference:** Simple, pure, instant.
- **JS-side materialization:** No TP based serialization backend, the graph is
  materialized in JS using normal engine query calls. Much simpler, just as
  effective. Also allows for live stats reporting such as cache hits and which
  node is currently materializing. Added a subtle glow/pulse effect on the
  currently materializing node.
- **SQL folding:** Nodes are folded into reasonable individual SQL statements,
  resulting in much more readable SQL code generation.
- **Stable configs:** When a node is detached from its parent it retains its
  internal configuration and visually it doesn't change size or shape at all. A
  detached node can be configured standalone, you just don't get column
  name/type hints. When attached to an upstream node, the fields don't change or
  anything, they just show a little warning if there is a column name / type
  mismatch.
- **Node style:**
  - **Color coded titlebars with always visible names:** This makes the graph
    much easier to interpret as there is a clear separation between the node
    title and its content, and provides a stable grab point which can be used to
    drag nodes around without accidentally clicking on the node inputs.
  - **Nodes are only draggable via their title bars:** Which allows for
    draggable elements within the body of the node.
  - **Inputs and outputs are constrained to the left and right edges**:
    Resulting in more pleasing connection beziers.
*/

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
  pinNode(nodeId: string | undefined): void;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Spaghetti';
  static readonly dependencies = [SqlModulesPlugin];
  static readonly description = 'A visual query builder for SQL modules.';

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
   * Validate a serialized graph JSON string without applying it.
   * Returns an array of error strings; empty means valid.
   */
  validateGraphJson(json: string): string[] {
    const errors: string[] = [];

    let obj: {nodes?: unknown; connections?: unknown};
    try {
      obj = JSON.parse(json);
    } catch (e) {
      return [`Invalid JSON: ${e}`];
    }

    if (
      !obj.nodes ||
      typeof obj.nodes !== 'object' ||
      Array.isArray(obj.nodes)
    ) {
      return ['nodes must be an object mapping id → NodeData'];
    }

    // Validate each node.
    const nodes: Record<string, NodeData> = {};
    for (const [id, node] of Object.entries(obj.nodes)) {
      if (typeof id !== 'string' || !id) {
        errors.push(
          `Node ID must be a non-empty string, got: ${JSON.stringify(id)}`,
        );
        continue;
      }
      if (!node || typeof node !== 'object') {
        errors.push(`Node "${id}" data must be an object`);
        continue;
      }
      const n = node as NodeData;
      if (!n.type || typeof n.type !== 'string') {
        errors.push(`Node "${id}" is missing a type string`);
        continue;
      }
      const manifest = getManifest(n.type);
      if (!manifest) {
        errors.push(`Node "${id}": unknown type "${n.type}"`);
        continue;
      }
      if (!isNodeValid(n)) {
        errors.push(`Node "${id}" (${n.type}): config is invalid`);
      }
      nodes[id] = n;
    }

    // Check nextId references.
    for (const [id, node] of Object.entries(nodes)) {
      if (node.nextId !== undefined && !(node.nextId in nodes)) {
        errors.push(
          `Node "${id}": nextId "${node.nextId}" does not reference a known node`,
        );
      }
    }

    // Check connections.
    const connections = Array.isArray(obj.connections) ? obj.connections : [];
    const connKeys = new Set<string>();
    for (let i = 0; i < connections.length; i++) {
      const c = connections[i] as {
        fromNode?: unknown;
        fromPort?: unknown;
        toNode?: unknown;
        toPort?: unknown;
      };
      const label = `Connection[${i}]`;
      if (typeof c.fromNode !== 'string' || !(c.fromNode in nodes)) {
        errors.push(`${label}: fromNode "${c.fromNode}" is not a known node`);
        continue;
      }
      if (typeof c.toNode !== 'string' || !(c.toNode in nodes)) {
        errors.push(`${label}: toNode "${c.toNode}" is not a known node`);
        continue;
      }

      // Duplicate connection check.
      const connKey = `${c.fromNode}:${c.fromPort}->${c.toNode}:${c.toPort}`;
      if (connKeys.has(connKey)) {
        errors.push(`${label}: duplicate connection ${connKey}`);
      }
      connKeys.add(connKey);

      // nextId + connection conflict: if fromNode already docks into toNode
      // via nextId, a connection between them is redundant and harmful.
      const fromNode = nodes[c.fromNode];
      if (fromNode.nextId === c.toNode) {
        errors.push(
          `${label}: node "${c.fromNode}" already docks into "${c.toNode}" ` +
            `via nextId — remove this connection or remove the nextId`,
        );
      }

      const toNode = nodes[c.toNode];
      const toManifest = getManifest(toNode.type);
      if (toManifest) {
        const ports = getManifestInputs(toManifest, toNode);
        const toPort = typeof c.toPort === 'number' ? c.toPort : -1;
        if (toPort < 0 || toPort >= ports.length) {
          errors.push(
            `${label}: toPort ${c.toPort} is out of range for node "${c.toNode}" ` +
              `(type "${toNode.type}" has ${ports.length} input port(s))`,
          );
        }
      }
    }

    return errors;
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
   * Pin a node by ID (or pass undefined to unpin).
   */
  pinNode(nodeId: string | undefined): void {
    this.delegate?.pinNode(nodeId);
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
