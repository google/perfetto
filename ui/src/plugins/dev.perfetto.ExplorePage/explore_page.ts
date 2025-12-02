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
import SqlModulesPlugin from '../dev.perfetto.SqlModules';

import {Builder} from './query_builder/builder';
import {
  QueryNode,
  QueryNodeState,
  NodeType,
  NodeActions,
  addConnection,
  removeConnection,
  singleNodeOperation,
} from './query_node';
import {UIFilter} from './query_builder/operations/filter';
import {Trace} from '../../public/trace';

import {exportStateAsJson, importStateFromJson} from './json_handler';
import {showImportWithStatementModal} from './sql_json_handler';
import {registerCoreNodes} from './query_builder/core_nodes';
import {nodeRegistry, PreCreateState} from './query_builder/node_registry';
import {QueryExecutionService} from './query_builder/query_execution_service';
import {CleanupManager} from './query_builder/cleanup_manager';
import {HistoryManager} from './history_manager';
import {
  getAllNodes,
  insertNodeBetween,
  reconnectParentsToChildren,
  getInputNodeAtPort,
  getAllInputNodes,
} from './query_builder/graph_utils';
import {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';

registerCoreNodes();

// Grid layout constants
const NODES_PER_ROW = 3;
const NODE_HORIZONTAL_SPACING = 250;
const NODE_VERTICAL_SPACING = 180;
const GRID_START_X = 100;
const GRID_START_Y = 100;

/**
 * Generates grid layout positions for nodes arranged in rows.
 *
 * @param nodes The nodes to layout
 * @returns Map of node IDs to {x, y} positions
 */
function createGridLayout(
  nodes: QueryNode[],
): Map<string, {x: number; y: number}> {
  const layouts = new Map<string, {x: number; y: number}>();

  nodes.forEach((node, index) => {
    const row = Math.floor(index / NODES_PER_ROW);
    const col = index % NODES_PER_ROW;

    const x = GRID_START_X + col * NODE_HORIZONTAL_SPACING;
    const y = GRID_START_Y + row * NODE_VERTICAL_SPACING;

    layouts.set(node.nodeId, {x, y});
  });

  return layouts;
}

/**
 * Creates slice source node and thread_state table node.
 * This is used for auto-initialization when the explore page first opens.
 *
 * @param sqlModules The SQL modules interface for accessing table metadata
 * @param trace The trace instance
 * @param allNodes All existing nodes in the graph
 * @returns Array of newly created nodes (slice source and thread_state table)
 */
function createHighImportanceTableNodes(
  sqlModules: SqlModules,
  trace: Trace,
  allNodes: QueryNode[],
): QueryNode[] {
  const newNodes: QueryNode[] = [];

  // Create slice source node
  const sliceDescriptor = nodeRegistry.get('slice');
  if (sliceDescriptor) {
    try {
      const sliceNode = sliceDescriptor.factory(
        {
          trace,
        },
        {allNodes},
      );
      newNodes.push(sliceNode);
    } catch (error) {
      console.error('Failed to create slice source node:', error);
    }
  }

  // Create thread_state table node
  const tableDescriptor = nodeRegistry.get('table');
  if (tableDescriptor) {
    const threadStateTable = sqlModules
      .listTables()
      .find((table) => table.name === 'thread_state');

    if (threadStateTable) {
      // Check if the table is available (module not disabled)
      if (
        !threadStateTable.includeKey ||
        !sqlModules.isModuleDisabled(threadStateTable.includeKey)
      ) {
        try {
          const threadStateNode = tableDescriptor.factory(
            {
              sqlTable: threadStateTable,
              sqlModules,
              trace,
            },
            {allNodes},
          );
          newNodes.push(threadStateNode);
        } catch (error) {
          console.error('Failed to create thread_state table node:', error);
        }
      }
    }
  }

  return newNodes;
}

export interface ExplorePageState {
  rootNodes: QueryNode[];
  selectedNode?: QueryNode;
  nodeLayouts: Map<string, {x: number; y: number}>;
  devMode?: boolean;
}

interface ExplorePageAttrs {
  readonly trace: Trace;
  readonly sqlModulesPlugin: SqlModulesPlugin;
  readonly state: ExplorePageState;
  readonly onStateUpdate: (
    update:
      | ExplorePageState
      | ((currentState: ExplorePageState) => ExplorePageState),
  ) => void;
}

export class ExplorePage implements m.ClassComponent<ExplorePageAttrs> {
  private queryExecutionService?: QueryExecutionService;
  private cleanupManager?: CleanupManager;
  private historyManager?: HistoryManager;
  private initializedNodes = new Set<string>();
  private hasAutoInitialized = false;

  private selectNode(attrs: ExplorePageAttrs, node: QueryNode) {
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      selectedNode: node,
    }));
  }

  private deselectNode(attrs: ExplorePageAttrs) {
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      selectedNode: undefined,
    }));
  }

  private createNodeActions(
    attrs: ExplorePageAttrs,
    node: QueryNode,
  ): NodeActions {
    return {
      onAddAndConnectTable: (tableName: string, portIndex: number) => {
        this.handleAddAndConnectTable(attrs, tableName, node, portIndex);
      },
      onInsertModifyColumnsNode: (portIndex: number) => {
        this.handleInsertModifyColumnsNode(attrs, node, portIndex);
      },
    };
  }

  private ensureNodeActions(attrs: ExplorePageAttrs, node: QueryNode) {
    // Skip if already initialized
    if (this.initializedNodes.has(node.nodeId)) {
      return;
    }

    // Initialize actions if not present
    if (!node.state.actions) {
      node.state.actions = this.createNodeActions(attrs, node);
    }

    // Mark as initialized
    this.initializedNodes.add(node.nodeId);
  }

  private async handleDevModeChange(attrs: ExplorePageAttrs, enabled: boolean) {
    if (enabled) {
      const {registerDevNodes} = await import('./query_builder/dev_nodes');
      registerDevNodes();
    }
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      devMode: enabled,
    }));
  }

  async handleAddOperationNode(
    attrs: ExplorePageAttrs,
    node: QueryNode,
    derivedNodeId: string,
  ): Promise<QueryNode | undefined> {
    const {state, onStateUpdate} = attrs;
    const descriptor = nodeRegistry.get(derivedNodeId);
    if (descriptor) {
      let initialState: PreCreateState | PreCreateState[] | null = {};
      if (descriptor.preCreate) {
        const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
        if (!sqlModules) return;
        initialState = await descriptor.preCreate({sqlModules});
      }

      if (initialState === null) {
        return;
      }

      // For operation nodes, we only support single node creation
      // (multi-select only makes sense for source nodes)
      if (Array.isArray(initialState)) {
        console.warn(
          'Operation nodes do not support multi-node creation from preCreate',
        );
        return;
      }

      const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
      if (!sqlModules) return;

      // Use a wrapper object to hold the node reference (allows mutation without 'let')
      const nodeRef: {current?: QueryNode} = {};

      const nodeState: QueryNodeState = {
        ...(initialState as Partial<QueryNodeState>),
        sqlModules,
        trace: attrs.trace,
        // Provide actions for nodes that need to interact with the graph
        // We use a closure pattern because the node doesn't exist yet
        actions: {
          onAddAndConnectTable: (tableName: string, portIndex: number) => {
            if (nodeRef.current !== undefined) {
              this.handleAddAndConnectTable(
                attrs,
                tableName,
                nodeRef.current,
                portIndex,
              );
            }
          },
          onInsertModifyColumnsNode: (portIndex: number) => {
            if (nodeRef.current !== undefined) {
              this.handleInsertModifyColumnsNode(
                attrs,
                nodeRef.current,
                portIndex,
              );
            }
          },
        },
      };

      const newNode = descriptor.factory(nodeState, {
        allNodes: state.rootNodes,
      });

      // Set the reference so the callback can use it
      nodeRef.current = newNode;

      // Mark this node as initialized
      this.initializedNodes.add(newNode.nodeId);

      if (singleNodeOperation(newNode.type)) {
        // For single-input operations: insert between the target and its children
        insertNodeBetween(node, newNode, addConnection, removeConnection);

        onStateUpdate((currentState) => ({
          ...currentState,
          selectedNode: newNode,
        }));
      } else {
        // For multi-source nodes: just connect and add to root nodes
        // Don't insert in-between - the node combines multiple sources
        addConnection(node, newNode);

        onStateUpdate((currentState) => ({
          ...currentState,
          rootNodes: [...currentState.rootNodes, newNode],
          selectedNode: newNode,
        }));
      }

      return newNode;
    }

    return undefined;
  }

  private async handleAddSourceNode(attrs: ExplorePageAttrs, id: string) {
    const descriptor = nodeRegistry.get(id);
    if (!descriptor) return;

    let initialState: PreCreateState | PreCreateState[] | null = {};

    if (descriptor.preCreate) {
      const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
      if (!sqlModules) return;
      initialState = await descriptor.preCreate({sqlModules});
    }

    if (initialState === null) {
      return;
    }

    // Handle both single node and multi-node creation
    const statesToCreate = Array.isArray(initialState)
      ? initialState
      : [initialState];

    const newNodes: QueryNode[] = [];
    for (const state of statesToCreate) {
      try {
        const newNode = descriptor.factory(
          {
            ...state,
            trace: attrs.trace,
          } as QueryNodeState,
          {allNodes: attrs.state.rootNodes},
        );
        newNodes.push(newNode);
      } catch (error) {
        console.error('Failed to create node:', error);
        // Continue creating other nodes even if one fails
      }
    }

    // If no nodes were successfully created, return early
    if (newNodes.length === 0) {
      return;
    }

    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [...currentState.rootNodes, ...newNodes],
      selectedNode: newNodes[newNodes.length - 1], // Select the last node
    }));
  }

  private autoInitializeHighImportanceTables(attrs: ExplorePageAttrs) {
    this.hasAutoInitialized = true;

    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
    if (!sqlModules) return;

    const newNodes = createHighImportanceTableNodes(
      sqlModules,
      attrs.trace,
      attrs.state.rootNodes,
    );

    // Add all nodes to the graph with grid layout
    if (newNodes.length > 0) {
      const gridLayouts = createGridLayout(newNodes);

      attrs.onStateUpdate((currentState) => {
        // Merge new layouts with existing layouts
        const newNodeLayouts = new Map(currentState.nodeLayouts);
        gridLayouts.forEach((layout, nodeId) => {
          newNodeLayouts.set(nodeId, layout);
        });

        return {
          ...currentState,
          rootNodes: [...currentState.rootNodes, ...newNodes],
          nodeLayouts: newNodeLayouts,
          // Don't select any node - leave selection empty
        };
      });
    }
  }

  private async handleAddAndConnectTable(
    attrs: ExplorePageAttrs,
    tableName: string,
    targetNode: QueryNode,
    portIndex: number,
  ) {
    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
    if (!sqlModules) return;

    // Get the table descriptor
    const descriptor = nodeRegistry.get('table');
    if (!descriptor) return;

    // Find the table in SQL modules
    const sqlTable = sqlModules.listTables().find((t) => t.name === tableName);
    if (!sqlTable) {
      console.warn(`Table ${tableName} not found in SQL modules`);
      return;
    }

    // Create the table node with the specific table (bypass the modal)
    const newNode = descriptor.factory(
      {
        sqlTable,
        sqlModules,
        trace: attrs.trace,
      },
      {allNodes: attrs.state.rootNodes},
    );

    // Add connection from the new table node to the target node
    addConnection(newNode, targetNode, portIndex);

    // Add the new node to root nodes
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [...currentState.rootNodes, newNode],
    }));
  }

  private async handleInsertModifyColumnsNode(
    attrs: ExplorePageAttrs,
    targetNode: QueryNode,
    portIndex: number,
  ) {
    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
    if (!sqlModules) return;

    // Get the ModifyColumns descriptor
    const descriptor = nodeRegistry.get('modify_columns');
    if (!descriptor) return;

    // Get the current input node at the specified port
    const inputNode = getInputNodeAtPort(targetNode, portIndex);

    if (!inputNode) {
      console.warn(`No input node found at port ${portIndex}`);
      return;
    }

    // Create the ModifyColumns node
    const newNode = descriptor.factory(
      {
        sqlModules,
        trace: attrs.trace,
      },
      {allNodes: attrs.state.rootNodes},
    );

    // Remove the old connection from inputNode to targetNode
    removeConnection(inputNode, targetNode);

    // Add connection from inputNode to ModifyColumns node (sets primaryInput)
    addConnection(inputNode, newNode);

    // Add connection from ModifyColumns node to targetNode at the same port
    addConnection(newNode, targetNode, portIndex);

    // Add the new node to root nodes (so it appears in the graph)
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [...currentState.rootNodes, newNode],
      selectedNode: newNode,
    }));
  }

  async handleClearAllNodes(attrs: ExplorePageAttrs) {
    // Clean up materialized tables for all nodes using CleanupManager
    if (this.cleanupManager !== undefined) {
      const allNodes = getAllNodes(attrs.state.rootNodes);
      await this.cleanupManager.cleanupNodes(allNodes);
    }

    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [],
      selectedNode: undefined,
    }));
  }

  handleDuplicateNode(attrs: ExplorePageAttrs, node: QueryNode) {
    const {onStateUpdate} = attrs;
    onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [...currentState.rootNodes, node.clone()],
    }));
  }

  /**
   * Helper to set filters on a node and optionally set the filter operator.
   * Reduces duplication across multiple filter-setting locations.
   */
  private setFiltersOnNode(
    node: QueryNode,
    filters: UIFilter[],
    filterOperator?: 'AND' | 'OR',
  ): void {
    node.state.filters = filters;
    if (filterOperator) {
      node.state.filterOperator = filterOperator;
    }
  }

  async handleFilterAdd(
    attrs: ExplorePageAttrs,
    sourceNode: QueryNode,
    filter: UIFilter | UIFilter[],
    filterOperator?: 'AND' | 'OR',
  ) {
    // Normalize to array for uniform handling (single filter → [filter])
    const filters: UIFilter[] = Array.isArray(filter) ? filter : [filter];

    // If the source node is already a FilterNode, just add the filter(s) to it
    if (sourceNode.type === NodeType.kFilter) {
      this.setFiltersOnNode(
        sourceNode,
        [...(sourceNode.state.filters ?? []), ...filters] as UIFilter[],
        filterOperator,
      );
      attrs.onStateUpdate((currentState) => ({...currentState}));
      return;
    }

    // If the source node has exactly one child and it's a FilterNode, add to that
    if (
      sourceNode.nextNodes.length === 1 &&
      sourceNode.nextNodes[0].type === NodeType.kFilter
    ) {
      const existingFilterNode = sourceNode.nextNodes[0];
      this.setFiltersOnNode(
        existingFilterNode,
        [...(existingFilterNode.state.filters ?? []), ...filters] as UIFilter[],
        filterOperator,
      );
      attrs.onStateUpdate((currentState) => ({
        ...currentState,
        selectedNode: existingFilterNode,
      }));
      return;
    }

    // Otherwise, create a new FilterNode after the source node
    const filterNodeId = 'filter_node';
    const newFilterNode = await this.handleAddOperationNode(
      attrs,
      sourceNode,
      filterNodeId,
    );

    // Add the filter(s) to the newly created FilterNode
    if (newFilterNode) {
      this.setFiltersOnNode(newFilterNode, filters, filterOperator);
      attrs.onStateUpdate((currentState) => ({
        ...currentState,
        selectedNode: newFilterNode,
      }));
    }
  }

  async handleDeleteNode(attrs: ExplorePageAttrs, node: QueryNode) {
    const {state, onStateUpdate} = attrs;

    // Clean up all node resources (both JS and SQL) using CleanupManager
    if (this.cleanupManager !== undefined) {
      await this.cleanupManager.cleanupNode(node);
    }

    let newRootNodes = state.rootNodes.filter((n) => n !== node);
    if (state.rootNodes.includes(node) && node.nextNodes.length > 0) {
      newRootNodes = [...newRootNodes, ...node.nextNodes];
    }

    // Get parent and child nodes before removing connections
    const parentNodes = getAllInputNodes(node);
    const childNodes = [...node.nextNodes];

    // Remove all connections to/from the deleted node
    for (const parent of parentNodes) {
      removeConnection(parent, node);
    }
    for (const child of childNodes) {
      removeConnection(node, child);
    }

    // Reconnect parents to children (bypass the deleted node)
    reconnectParentsToChildren(parentNodes, childNodes, addConnection);

    // If the deleted node was selected, deselect it.
    const newSelectedNode =
      state.selectedNode === node ? undefined : state.selectedNode;

    onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: newRootNodes,
      selectedNode: newSelectedNode,
    }));
  }

  handleConnectionRemove(
    attrs: ExplorePageAttrs,
    fromNode: QueryNode,
    toNode: QueryNode,
  ) {
    const {state, onStateUpdate} = attrs;

    // NOTE: The basic connection removal is already handled by graph.ts
    // This callback handles higher-level logic like reconnection and state updates

    // Check if we should reconnect fromNode to toNode's children (bypass toNode)
    // Note: We check if fromNode has no next nodes (connection already removed)
    const shouldReconnect =
      fromNode.nextNodes.length === 0 && toNode.nextNodes.length > 0;

    if (shouldReconnect) {
      // Reconnect fromNode to all of toNode's children (bypass toNode)
      for (const child of toNode.nextNodes) {
        addConnection(fromNode, child);
      }
    }

    // Handle state updates based on node type
    if ('primaryInput' in toNode && toNode.primaryInput === undefined) {
      // toNode is a ModificationNode that's now orphaned
      // Add it to rootNodes so it remains visible (but invalid)
      const newRootNodes = state.rootNodes.includes(toNode)
        ? state.rootNodes
        : [...state.rootNodes, toNode];

      onStateUpdate((currentState) => ({
        ...currentState,
        rootNodes: newRootNodes,
      }));
    } else if ('secondaryInputs' in toNode) {
      // toNode is a MultiSourceNode - just trigger a state update
      onStateUpdate((currentState) => ({...currentState}));
    }
  }

  handleExport(state: ExplorePageState, trace: Trace) {
    exportStateAsJson(state, trace);
  }

  handleImport(attrs: ExplorePageAttrs) {
    const {trace, sqlModulesPlugin, onStateUpdate} = attrs;
    const sqlModules = sqlModulesPlugin.getSqlModules();
    if (!sqlModules) return;

    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.json';
    input.onchange = (event) => {
      const files = (event.target as HTMLInputElement).files;
      if (files && files.length > 0) {
        const file = files[0];
        importStateFromJson(
          file,
          trace,
          sqlModules,
          (newState: ExplorePageState) => {
            onStateUpdate(newState);
          },
        );
      }
    };
    input.click();
  }

  private handleKeyDown(event: KeyboardEvent, attrs: ExplorePageAttrs) {
    const {state} = attrs;
    if (state.selectedNode) {
      return;
    }
    // Do not interfere with text inputs
    if (
      event.target instanceof HTMLInputElement ||
      event.target instanceof HTMLTextAreaElement
    ) {
      return;
    }

    // Handle undo/redo shortcuts
    if ((event.ctrlKey || event.metaKey) && event.key === 'z') {
      if (event.shiftKey) {
        // Ctrl+Shift+Z or Cmd+Shift+Z for Redo
        this.handleRedo(attrs);
        event.preventDefault();
        return;
      } else {
        // Ctrl+Z or Cmd+Z for Undo
        this.handleUndo(attrs);
        event.preventDefault();
        return;
      }
    }

    // Also support Ctrl+Y for Redo on Windows/Linux
    if ((event.ctrlKey || event.metaKey) && event.key === 'y') {
      this.handleRedo(attrs);
      event.preventDefault();
      return;
    }

    // Handle source node creation shortcuts
    for (const [id, descriptor] of nodeRegistry.list()) {
      if (
        descriptor.type === 'source' &&
        descriptor.hotkey &&
        event.key.toLowerCase() === descriptor.hotkey.toLowerCase()
      ) {
        this.handleAddSourceNode(attrs, id);
        event.preventDefault(); // Prevent default browser actions for this key
        return;
      }
    }

    // Handle other shortcuts
    switch (event.key) {
      case 'i':
        this.handleImport(attrs);
        break;
      case 'e':
        this.handleExport(attrs.state, attrs.trace);
        break;
    }
  }

  private handleImportWithStatement(attrs: ExplorePageAttrs) {
    const {trace, sqlModulesPlugin, onStateUpdate} = attrs;
    const sqlModules = sqlModulesPlugin.getSqlModules();
    if (!sqlModules) return;

    showImportWithStatementModal(trace, sqlModules, onStateUpdate);
  }

  private handleUndo(attrs: ExplorePageAttrs) {
    if (!this.historyManager) return;

    const previousState = this.historyManager.undo();
    if (previousState) {
      attrs.onStateUpdate(previousState);
    }
  }

  private handleRedo(attrs: ExplorePageAttrs) {
    if (!this.historyManager) return;

    const nextState = this.historyManager.redo();
    if (nextState) {
      attrs.onStateUpdate(nextState);
    }
  }

  view({attrs}: m.CVnode<ExplorePageAttrs>) {
    const {trace, state} = attrs;

    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();

    if (!sqlModules) {
      return m(
        '.pf-explore-page',
        m(
          '.pf-explore-page__header',
          m('h1', 'Loading SQL Modules, please wait...'),
        ),
      );
    }

    // Initialize history manager if not already done
    if (!this.historyManager) {
      this.historyManager = new HistoryManager(trace, sqlModules);
      // Push initial state
      this.historyManager.pushState(state);
    }

    // Wrap onStateUpdate to track history
    const wrappedOnStateUpdate = (
      update:
        | ExplorePageState
        | ((currentState: ExplorePageState) => ExplorePageState),
    ) => {
      attrs.onStateUpdate((currentState) => {
        const newState =
          typeof update === 'function' ? update(currentState) : update;
        // Push state to history after update
        this.historyManager?.pushState(newState);
        return newState;
      });
    };

    // Create wrapped attrs to track history
    const wrappedAttrs = {
      ...attrs,
      onStateUpdate: wrappedOnStateUpdate,
    };

    // Ensure all nodes have actions initialized (e.g., nodes from imported state)
    // This is efficient - only processes nodes not yet initialized
    const allNodes = getAllNodes(state.rootNodes);
    for (const node of allNodes) {
      this.ensureNodeActions(wrappedAttrs, node);
    }

    // Initialize services if not already done
    if (this.queryExecutionService === undefined) {
      this.queryExecutionService = new QueryExecutionService(
        attrs.trace.engine,
      );
      this.cleanupManager = new CleanupManager(this.queryExecutionService);
    }

    // Auto-initialize high-importance tables on first load
    if (state.rootNodes.length === 0 && !this.hasAutoInitialized) {
      this.autoInitializeHighImportanceTables(wrappedAttrs);
    }

    return m(
      '.pf-explore-page',
      {
        onkeydown: (e: KeyboardEvent) => this.handleKeyDown(e, wrappedAttrs),
        oncreate: (vnode) => {
          (vnode.dom as HTMLElement).focus();
        },
        onremove: async () => {
          // Clean up all materialized tables when component is destroyed
          if (this.cleanupManager !== undefined) {
            const allNodes = getAllNodes(state.rootNodes);
            await this.cleanupManager.cleanupAll(allNodes);
          }
        },
        tabindex: 0,
      },
      m(Builder, {
        trace,
        sqlModules,
        queryExecutionService: this.queryExecutionService,
        rootNodes: state.rootNodes,
        selectedNode: state.selectedNode,
        nodeLayouts: state.nodeLayouts,
        devMode: state.devMode,
        onDevModeChange: (enabled) =>
          this.handleDevModeChange(wrappedAttrs, enabled),
        onRootNodeCreated: (node) => {
          wrappedAttrs.onStateUpdate((currentState) => ({
            ...currentState,
            rootNodes: [...currentState.rootNodes, node],
            selectedNode: node,
          }));
        },
        onNodeSelected: (node) => {
          if (node) this.selectNode(wrappedAttrs, node);
        },
        onDeselect: () => this.deselectNode(wrappedAttrs),
        onNodeLayoutChange: (nodeId, layout) => {
          wrappedAttrs.onStateUpdate((currentState) => {
            const newNodeLayouts = new Map(currentState.nodeLayouts);
            newNodeLayouts.set(nodeId, layout);
            return {
              ...currentState,
              nodeLayouts: newNodeLayouts,
            };
          });
        },
        onAddSourceNode: (id) => {
          this.handleAddSourceNode(wrappedAttrs, id);
        },
        onAddOperationNode: (id, node) => {
          this.handleAddOperationNode(wrappedAttrs, node, id);
        },
        onClearAllNodes: () => this.handleClearAllNodes(wrappedAttrs),
        onDuplicateNode: () => {
          if (state.selectedNode) {
            this.handleDuplicateNode(wrappedAttrs, state.selectedNode);
          }
        },
        onDeleteNode: () => {
          if (state.selectedNode) {
            this.handleDeleteNode(wrappedAttrs, state.selectedNode);
          }
        },
        onConnectionRemove: (fromNode, toNode) => {
          this.handleConnectionRemove(wrappedAttrs, fromNode, toNode);
        },
        onImport: () => this.handleImport(wrappedAttrs),
        onImportWithStatement: () =>
          this.handleImportWithStatement(wrappedAttrs),
        onExport: () => this.handleExport(state, trace),
        onFilterAdd: (node, filter, filterOperator) => {
          this.handleFilterAdd(wrappedAttrs, node, filter, filterOperator);
        },
        onNodeStateChange: () => {
          // Trigger a state update when node properties change (e.g., selecting group by columns)
          // This ensures these granular changes are captured in history
          wrappedAttrs.onStateUpdate((currentState) => {
            return {...currentState};
          });
        },
        onUndo: () => this.handleUndo(attrs),
        onRedo: () => this.handleRedo(attrs),
        canUndo: this.historyManager?.canUndo() ?? false,
        canRedo: this.historyManager?.canRedo() ?? false,
      }),
    );
  }
}
