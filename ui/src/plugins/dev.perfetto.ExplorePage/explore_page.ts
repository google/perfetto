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
import {assetSrc} from '../../base/assets';
import {showModal} from '../../widgets/modal';

import {Builder} from './query_builder/builder';
import {
  QueryNode,
  QueryNodeState,
  NodeType,
  NodeActions,
  singleNodeOperation,
} from './query_node';
import {UIFilter} from './query_builder/operations/filter';
import {FilterNode} from './query_builder/nodes/filter_node';
import {AddColumnsNode} from './query_builder/nodes/add_columns_node';
import {SlicesSourceNode} from './query_builder/nodes/sources/slices_source';
import {Column} from '../../components/widgets/datagrid/model';
import {Trace} from '../../public/trace';

import {exportStateAsJson, deserializeState} from './json_handler';
import {registerCoreNodes} from './query_builder/core_nodes';
import {nodeRegistry, PreCreateState} from './query_builder/node_registry';
import {QueryExecutionService} from './query_builder/query_execution_service';
import {CleanupManager} from './query_builder/cleanup_manager';
import {HistoryManager} from './history_manager';
import {getPrimarySelectedNode} from './selection_utils';
import {
  getAllNodes,
  insertNodeBetween,
  getInputNodeAtPort,
  getAllInputNodes,
  findDockedChildren,
  calculateUndockLayouts,
  getEffectiveLayout,
  addConnection,
  removeConnection,
  notifyNextNodes,
  captureAllChildConnections,
} from './query_builder/graph_utils';
import {
  showStateOverwriteWarning,
  showExportWarning,
} from './query_builder/widgets';
import {recentGraphsStorage} from './recent_graphs';
import {showDataExplorerHelp} from './data_explorer_help_modal';

registerCoreNodes();

// Clipboard entry stores a cloned node with its relative position for paste
interface ClipboardEntry {
  node: QueryNode;
  relativeX: number; // Position relative to the first node (only used if not docked)
  relativeY: number;
  isDocked: boolean; // True if node was docked (no explicit layout position)
}

// Clipboard connection stores connections between clipboard nodes (by index)
interface ClipboardConnection {
  fromIndex: number;
  toIndex: number;
  portIndex?: number;
}

export interface ExplorePageState {
  rootNodes: QueryNode[];
  selectedNodes: ReadonlySet<string>; // Set of selected node IDs for multi-selection
  nodeLayouts: Map<string, {x: number; y: number}>;
  labels: Array<{
    id: string;
    x: number;
    y: number;
    width: number;
    text: string;
  }>;
  isExplorerCollapsed?: boolean;
  sidebarWidth?: number;
  loadGeneration?: number; // Incremented each time content is loaded
  // Clipboard for multi-node copy/paste
  clipboardNodes?: ClipboardEntry[];
  clipboardConnections?: ClipboardConnection[];
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
  readonly hasAutoInitialized: boolean;
  readonly setHasAutoInitialized: (value: boolean) => void;
}

export class ExplorePage implements m.ClassComponent<ExplorePageAttrs> {
  private queryExecutionService?: QueryExecutionService;
  private cleanupManager?: CleanupManager;
  private historyManager?: HistoryManager;
  private initializedNodes = new Set<string>();
  private executeFn?: () => Promise<void>;

  /**
   * Shows confirmation dialog if there are unsaved changes, and finalizes
   * the current graph before loading a new one. Returns true if the user
   * confirmed (or there was nothing to confirm), false if cancelled.
   */
  private async confirmAndFinalizeCurrentGraph(
    state: ExplorePageState,
  ): Promise<boolean> {
    if (state.rootNodes.length > 0 || state.labels.length > 0) {
      const confirmed = await showStateOverwriteWarning();
      if (!confirmed) return false;
      recentGraphsStorage.finalizeCurrentGraph();
    }
    return true;
  }

  private selectNode(attrs: ExplorePageAttrs, node: QueryNode) {
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      selectedNodes: new Set([node.nodeId]),
    }));
  }

  private addNodeToSelection(attrs: ExplorePageAttrs, node: QueryNode) {
    attrs.onStateUpdate((currentState) => {
      const newSelectedNodes = new Set(currentState.selectedNodes);
      newSelectedNodes.add(node.nodeId);
      return {
        ...currentState,
        selectedNodes: newSelectedNodes,
      };
    });
  }

  private removeNodeFromSelection(attrs: ExplorePageAttrs, nodeId: string) {
    attrs.onStateUpdate((currentState) => {
      const newSelectedNodes = new Set(currentState.selectedNodes);
      newSelectedNodes.delete(nodeId);
      return {
        ...currentState,
        selectedNodes: newSelectedNodes,
      };
    });
  }

  private deselectNode(attrs: ExplorePageAttrs) {
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      selectedNodes: new Set(),
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
      onInsertCounterToIntervalsNode: (portIndex: number) => {
        this.handleInsertCounterToIntervalsNode(attrs, node, portIndex);
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
        if (!sqlModules) {
          console.warn('Cannot add operation node: SQL modules not loaded yet');
          return;
        }
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
      if (!sqlModules) {
        console.warn('Cannot add operation node: SQL modules not loaded yet');
        return;
      }

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
          onInsertCounterToIntervalsNode: (portIndex: number) => {
            if (nodeRef.current !== undefined) {
              this.handleInsertCounterToIntervalsNode(
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
          selectedNodes: new Set([newNode.nodeId]),
        }));
      } else {
        // For multi-source nodes: just connect and add to root nodes
        // Don't insert in-between - the node combines multiple sources

        // Undock docked children before adding (docking requires exactly one child)
        const dockedChildren = findDockedChildren(node, state.nodeLayouts);

        addConnection(node, newNode);

        onStateUpdate((currentState) => {
          const updatedLayouts = new Map(currentState.nodeLayouts);

          // Undock existing docked children by giving them layouts.
          // Use getEffectiveLayout to handle the case where the parent node is
          // itself docked (no direct layout) - we walk up the chain to find
          // the first ancestor with a layout.
          const effectiveLayout = getEffectiveLayout(
            node,
            currentState.nodeLayouts,
          );
          if (effectiveLayout !== undefined && dockedChildren.length > 0) {
            const undockLayouts = calculateUndockLayouts(
              dockedChildren,
              effectiveLayout,
            );
            for (const [nodeId, layout] of undockLayouts) {
              updatedLayouts.set(nodeId, layout);
            }
          }

          return {
            ...currentState,
            rootNodes: [...currentState.rootNodes, newNode],
            nodeLayouts: updatedLayouts,
            selectedNodes: new Set([newNode.nodeId]),
          };
        });
      }

      return newNode;
    }

    console.warn(
      `Cannot add operation node: unknown type '${derivedNodeId}' for source node ${node.nodeId}`,
    );
    return undefined;
  }

  private async handleAddSourceNode(attrs: ExplorePageAttrs, id: string) {
    const descriptor = nodeRegistry.get(id);
    if (!descriptor) {
      console.warn(`Cannot add source node: unknown node type '${id}'`);
      return;
    }

    let initialState: PreCreateState | PreCreateState[] | null = {};

    if (descriptor.preCreate) {
      const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
      if (!sqlModules) {
        console.warn('Cannot add source node: SQL modules not loaded yet');
        return;
      }
      initialState = await descriptor.preCreate({sqlModules});
    }

    // User cancelled the preCreate dialog
    if (initialState === null) {
      return;
    }

    // Handle both single node and multi-node creation
    const statesToCreate = Array.isArray(initialState)
      ? initialState
      : [initialState];

    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
    const newNodes: QueryNode[] = [];
    for (const state of statesToCreate) {
      try {
        const newNode = descriptor.factory(
          {
            ...state,
            trace: attrs.trace,
            sqlModules,
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
    // (errors were already logged in the try-catch above)
    if (newNodes.length === 0) {
      console.warn('No nodes were created from the preCreate result');
      return;
    }

    const lastNode = newNodes[newNodes.length - 1];
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [...currentState.rootNodes, ...newNodes],
      selectedNodes: new Set([lastNode.nodeId]),
    }));
  }

  private async createExploreGraph(attrs: ExplorePageAttrs) {
    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
    if (!sqlModules) return;

    const newNodes: QueryNode[] = [];

    // Create slices source node
    const slicesNode = new SlicesSourceNode({sqlModules, trace: attrs.trace});
    newNodes.push(slicesNode);

    // Get high-frequency tables with data
    const tableDescriptor = nodeRegistry.get('table');
    if (tableDescriptor) {
      const highFreqTables = sqlModules
        .listTables()
        .filter((table) => table.importance === 'high');

      for (const sqlTable of highFreqTables) {
        try {
          // Check if the module is disabled (no data available)
          const module = sqlModules.getModuleForTable(sqlTable.name);
          if (module && sqlModules.isModuleDisabled(module.includeKey)) {
            continue; // Skip tables from disabled modules
          }

          const tableNode = tableDescriptor.factory(
            {
              sqlTable,
              sqlModules,
              trace: attrs.trace,
            },
            {allNodes: attrs.state.rootNodes},
          );
          newNodes.push(tableNode);
        } catch (error) {
          console.error(
            `Failed to create table node for ${sqlTable.name}:`,
            error,
          );
        }
      }
    }

    // Add all nodes to root nodes with grid layout
    if (newNodes.length > 0) {
      // Calculate grid dimensions (as square as possible)
      const totalNodes = newNodes.length;
      const cols = Math.ceil(Math.sqrt(totalNodes));

      // Create layout map with grid positions
      const newNodeLayouts = new Map();
      const NODE_WIDTH = 300;
      const NODE_HEIGHT = 200;
      const GRID_PADDING_X = 10;
      const GRID_PADDING_Y = 10;
      const START_X = 50;
      const START_Y = 50;

      newNodes.forEach((node, index) => {
        const col = index % cols;
        const row = Math.floor(index / cols);
        newNodeLayouts.set(node.nodeId, {
          x: START_X + col * (NODE_WIDTH + GRID_PADDING_X),
          y: START_Y + row * (NODE_HEIGHT + GRID_PADDING_Y),
        });
      });

      // Atomically update state with new nodes and incremented loadGeneration
      attrs.onStateUpdate((currentState) => ({
        ...currentState,
        rootNodes: newNodes, // Replace all nodes
        nodeLayouts: newNodeLayouts,
        selectedNodes: new Set([newNodes[0].nodeId]),
        labels: [], // Clear labels
        loadGeneration: (currentState.loadGeneration ?? 0) + 1,
      }));
    }
  }

  private async autoInitializeHighImportanceTables(attrs: ExplorePageAttrs) {
    attrs.setHasAutoInitialized(true);

    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
    if (!sqlModules) {
      console.warn('Cannot auto-initialize tables: SQL modules not loaded yet');
      return;
    }

    try {
      // Load the base page state from JSON
      const response = await fetch(
        assetSrc('assets/explore_page/base-page.json'),
      );
      if (!response.ok) {
        console.warn(
          'Failed to load base page state, falling back to empty state',
        );
        return;
      }
      const json = await response.text();
      const newState = deserializeState(json, attrs.trace, sqlModules);
      // Atomically update state with incremented loadGeneration
      attrs.onStateUpdate((currentState) => ({
        ...newState,
        loadGeneration: (currentState.loadGeneration ?? 0) + 1,
      }));
    } catch (error) {
      console.error('Failed to load base page state:', error);
      // Silently fail - leave the page empty if JSON can't be loaded
    }
  }

  private async handleAddAndConnectTable(
    attrs: ExplorePageAttrs,
    tableName: string,
    targetNode: QueryNode,
    portIndex: number,
  ) {
    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
    if (!sqlModules) {
      console.warn('Cannot add table: SQL modules not loaded yet');
      return;
    }

    // Get the table descriptor
    const descriptor = nodeRegistry.get('table');
    if (!descriptor) {
      console.warn("Cannot add table: 'table' node type not found in registry");
      return;
    }

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
    if (!sqlModules) {
      console.warn('Cannot insert modify columns node: SQL modules not loaded');
      return;
    }

    // Get the ModifyColumns descriptor
    const descriptor = nodeRegistry.get('modify_columns');
    if (!descriptor) {
      console.warn(
        "Cannot insert modify columns node: 'modify_columns' node type not found in registry",
      );
      return;
    }

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
      selectedNodes: new Set([newNode.nodeId]),
    }));
  }

  private async handleInsertCounterToIntervalsNode(
    attrs: ExplorePageAttrs,
    targetNode: QueryNode,
    portIndex: number,
  ) {
    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
    if (!sqlModules) {
      console.warn(
        'Cannot insert counter to intervals node: SQL modules not loaded',
      );
      return;
    }

    // Get the CounterToIntervals descriptor
    const descriptor = nodeRegistry.get('counter_to_intervals');
    if (!descriptor) {
      console.warn(
        "Cannot insert counter to intervals node: 'counter_to_intervals' node type not found in registry",
      );
      return;
    }

    // Get the current input node at the specified port
    const inputNode = getInputNodeAtPort(targetNode, portIndex);

    if (!inputNode) {
      console.warn(`No input node found at port ${portIndex}`);
      return;
    }

    // Create the CounterToIntervals node
    const newNode = descriptor.factory(
      {
        sqlModules,
        trace: attrs.trace,
      },
      {allNodes: attrs.state.rootNodes},
    );

    // Remove the old connection from inputNode to targetNode
    removeConnection(inputNode, targetNode);

    // Add connection from inputNode to CounterToIntervals node (sets primaryInput)
    addConnection(inputNode, newNode);

    // Add connection from CounterToIntervals node to targetNode at the same port
    addConnection(newNode, targetNode, portIndex);

    // Add the new node to root nodes (so it appears in the graph)
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [...currentState.rootNodes, newNode],
      selectedNodes: new Set([newNode.nodeId]),
    }));
  }

  /**
   * Cleans up all existing nodes (drops materialized tables) and clears
   * the initialized nodes set. Used when replacing the entire graph state.
   */
  private async cleanupExistingNodes(rootNodes: QueryNode[]) {
    if (this.cleanupManager !== undefined) {
      const allNodes = getAllNodes(rootNodes);
      await this.cleanupManager.cleanupNodes(allNodes);
    }
    this.initializedNodes.clear();
  }

  async handleClearAllNodes(attrs: ExplorePageAttrs) {
    await this.cleanupExistingNodes(attrs.state.rootNodes);

    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [],
      selectedNodes: new Set(),
      nodeLayouts: new Map(),
      labels: [],
    }));
  }

  handleDuplicateNode(attrs: ExplorePageAttrs, node: QueryNode) {
    const {onStateUpdate} = attrs;
    onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [...currentState.rootNodes, node.clone()],
    }));
  }

  private handleCopy(attrs: ExplorePageAttrs): void {
    const {state} = attrs;
    const selectedNodeIds = state.selectedNodes;

    if (selectedNodeIds.size === 0) {
      return;
    }

    const allNodes = getAllNodes(state.rootNodes);
    const selectedNodes = allNodes.filter((n) => selectedNodeIds.has(n.nodeId));

    if (selectedNodes.length === 0) {
      return;
    }

    // Get positions for relative layout calculation
    const positions = selectedNodes.map((node) => {
      const layout = state.nodeLayouts.get(node.nodeId);
      return {
        node,
        x: layout?.x ?? 0,
        y: layout?.y ?? 0,
      };
    });

    // Find the top-left corner as reference point
    const minX = Math.min(...positions.map((p) => p.x));
    const minY = Math.min(...positions.map((p) => p.y));

    // Create clipboard entries with cloned nodes and relative positions
    // Track whether each node is docked (no explicit layout) or undocked
    const nodeIdToIndex = new Map<string, number>();
    const clipboardNodes: ClipboardEntry[] = positions.map((p, index) => {
      nodeIdToIndex.set(p.node.nodeId, index);
      const hasLayout = state.nodeLayouts.has(p.node.nodeId);
      return {
        node: p.node.clone(),
        relativeX: p.x - minX,
        relativeY: p.y - minY,
        isDocked: !hasLayout,
      };
    });

    // Capture connections between selected nodes
    const clipboardConnections: ClipboardConnection[] = [];
    for (const node of selectedNodes) {
      const toIndex = nodeIdToIndex.get(node.nodeId);
      if (toIndex === undefined) continue;

      // Check primaryInput
      if (node.primaryInput && selectedNodeIds.has(node.primaryInput.nodeId)) {
        const fromIndex = nodeIdToIndex.get(node.primaryInput.nodeId);
        if (fromIndex !== undefined) {
          clipboardConnections.push({fromIndex, toIndex});
        }
      }

      // Check secondaryInputs
      if (node.secondaryInputs) {
        for (const [portIndex, inputNode] of node.secondaryInputs.connections) {
          if (selectedNodeIds.has(inputNode.nodeId)) {
            const fromIndex = nodeIdToIndex.get(inputNode.nodeId);
            if (fromIndex !== undefined) {
              clipboardConnections.push({fromIndex, toIndex, portIndex});
            }
          }
        }
      }
    }

    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      clipboardNodes,
      clipboardConnections,
    }));
  }

  private handlePaste(attrs: ExplorePageAttrs): void {
    const {state, onStateUpdate} = attrs;
    if (
      state.clipboardNodes === undefined ||
      state.clipboardNodes.length === 0
    ) {
      return;
    }

    onStateUpdate((currentState) => {
      if (
        currentState.clipboardNodes === undefined ||
        currentState.clipboardNodes.length === 0
      ) {
        return currentState;
      }

      // Clone nodes again for this paste operation (allows multiple pastes)
      const newNodes = currentState.clipboardNodes.map((entry) =>
        entry.node.clone(),
      );

      // Calculate paste offset (place slightly offset from original)
      const pasteOffsetX = 50;
      const pasteOffsetY = 50;

      // Update layouts for new nodes - only add layouts for undocked nodes
      // Docked nodes will remain docked (attached to their parent)
      const updatedLayouts = new Map(currentState.nodeLayouts);
      currentState.clipboardNodes.forEach((entry, index) => {
        if (!entry.isDocked) {
          updatedLayouts.set(newNodes[index].nodeId, {
            x: entry.relativeX + pasteOffsetX,
            y: entry.relativeY + pasteOffsetY,
          });
        }
      });

      // Restore connections between pasted nodes
      if (currentState.clipboardConnections) {
        for (const conn of currentState.clipboardConnections) {
          const fromNode = newNodes[conn.fromIndex] as QueryNode | undefined;
          const toNode = newNodes[conn.toIndex] as QueryNode | undefined;
          if (fromNode !== undefined && toNode !== undefined) {
            addConnection(fromNode, toNode, conn.portIndex);
          }
        }
      }

      // Select all newly pasted nodes
      const newSelectedNodes = new Set(newNodes.map((n) => n.nodeId));

      return {
        ...currentState,
        rootNodes: [...currentState.rootNodes, ...newNodes],
        selectedNodes: newSelectedNodes,
        nodeLayouts: updatedLayouts,
      };
    });
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
        selectedNodes: new Set([existingFilterNode.nodeId]),
      }));
      return;
    }

    // Otherwise, create a new FilterNode after the source node
    // Create it with filters already configured to avoid multiple undo points
    const newFilterNode = new FilterNode({
      filters,
      filterOperator,
      sqlModules: attrs.sqlModulesPlugin.getSqlModules(),
    });

    // Mark as initialized
    this.initializedNodes.add(newFilterNode.nodeId);

    // Insert between source node and its children
    insertNodeBetween(
      sourceNode,
      newFilterNode,
      addConnection,
      removeConnection,
    );

    // Single state update records the entire operation (node + filters)
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      selectedNodes: new Set([newFilterNode.nodeId]),
    }));
  }

  /**
   * Parses a column field to extract joinid information.
   * Returns undefined if the field is not a joinid column reference.
   */
  private parseJoinidColumnField(
    field: string,
    sourceNode: QueryNode,
  ):
    | {
        joinidColumnName: string;
        targetColumnName: string;
        targetTable: string;
        targetJoinColumn: string;
      }
    | undefined {
    // Parse the field to extract joinid column name and target column
    // Expected format: "joinidColumnName.targetColumnName"
    const dotIndex = field.indexOf('.');
    if (dotIndex === -1) {
      return undefined;
    }

    const joinidColumnName = field.substring(0, dotIndex);
    const targetColumnName = field.substring(dotIndex + 1);

    // Find the joinid column in the source node's finalCols
    const joinidColumnInfo = sourceNode.finalCols.find(
      (col) => col.name === joinidColumnName,
    );

    if (
      joinidColumnInfo === undefined ||
      joinidColumnInfo.column.type?.kind !== 'joinid'
    ) {
      return undefined;
    }

    return {
      joinidColumnName,
      targetColumnName,
      targetTable: joinidColumnInfo.column.type.source.table,
      targetJoinColumn: joinidColumnInfo.column.type.source.column,
    };
  }

  /**
   * Finds an AddColumnsNode that matches the given join configuration.
   * Checks both the source node itself and its immediate child.
   */
  private findMatchingAddColumnsNode(
    sourceNode: QueryNode,
    joinidColumnName: string,
    targetJoinColumn: string,
  ): AddColumnsNode | undefined {
    // Check if the source node is already an AddColumnsNode with the same join
    if (sourceNode.type === NodeType.kAddColumns) {
      const addColumnsNode = sourceNode as AddColumnsNode;
      if (
        addColumnsNode.state.leftColumn === joinidColumnName &&
        addColumnsNode.state.rightColumn === targetJoinColumn
      ) {
        return addColumnsNode;
      }
    }

    // Check if the source node has exactly one child that's an AddColumnsNode with same join
    if (
      sourceNode.nextNodes.length === 1 &&
      sourceNode.nextNodes[0].type === NodeType.kAddColumns
    ) {
      const existingAddColumnsNode = sourceNode.nextNodes[0] as AddColumnsNode;
      if (
        existingAddColumnsNode.state.leftColumn === joinidColumnName &&
        existingAddColumnsNode.state.rightColumn === targetJoinColumn
      ) {
        return existingAddColumnsNode;
      }
    }

    return undefined;
  }

  /**
   * Handles adding a column from a joinid table by creating an AddColumnsNode.
   * The column field is expected to be in the format "joinidColumn.targetColumnName"
   * where joinidColumn is a column with joinid type in the source node.
   */
  handleColumnAdd(
    attrs: ExplorePageAttrs,
    sourceNode: QueryNode,
    column: Column,
  ) {
    const parsed = this.parseJoinidColumnField(column.field, sourceNode);
    if (parsed === undefined) {
      // Not a joinid column reference - nothing to do
      return;
    }

    const {joinidColumnName, targetColumnName, targetTable, targetJoinColumn} =
      parsed;

    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
    if (sqlModules === undefined) {
      console.warn('Cannot add column: SQL modules not loaded yet');
      return;
    }

    // Check if this column name already exists in the source node's schema
    const existingColumnNames = new Set(
      sourceNode.finalCols.map((col) => col.name),
    );
    if (existingColumnNames.has(targetColumnName)) {
      console.warn(
        `Cannot add column: "${targetColumnName}" already exists in the schema`,
      );
      return;
    }

    // Try to find an existing AddColumnsNode with the same join configuration
    const existingNode = this.findMatchingAddColumnsNode(
      sourceNode,
      joinidColumnName,
      targetJoinColumn,
    );

    if (existingNode !== undefined) {
      // Check if the column is already added
      if (existingNode.state.selectedColumns?.includes(targetColumnName)) {
        console.warn(
          `Cannot add column: "${targetColumnName}" is already added`,
        );
        return;
      }

      // Add the column to the existing AddColumnsNode
      existingNode.state.selectedColumns = [
        ...(existingNode.state.selectedColumns ?? []),
        targetColumnName,
      ];
      existingNode.state.onchange?.();
      if (existingNode !== sourceNode) {
        attrs.onStateUpdate((currentState) => ({
          ...currentState,
          selectedNodes: new Set([existingNode.nodeId]),
        }));
      }
      return;
    }

    // Create a new AddColumnsNode with the join configuration
    // Note: selectedColumns is set after connecting the table node because
    // onPrevNodesUpdated() resets selectedColumns when rightNode is not connected
    const newAddColumnsNode = new AddColumnsNode({
      leftColumn: joinidColumnName,
      rightColumn: targetJoinColumn,
      isGuidedConnection: true,
      sqlModules,
      trace: attrs.trace,
    });

    // Set actions now that the node is created
    newAddColumnsNode.state.actions = this.createNodeActions(
      attrs,
      newAddColumnsNode,
    );

    // Mark as initialized
    this.initializedNodes.add(newAddColumnsNode.nodeId);

    // Insert between source node and its children
    insertNodeBetween(
      sourceNode,
      newAddColumnsNode,
      addConnection,
      removeConnection,
    );

    // Now create and connect the table source node
    const descriptor = nodeRegistry.get('table');
    if (descriptor === undefined) {
      console.warn("Cannot add table: 'table' node type not found in registry");
      return;
    }

    const sqlTable = sqlModules
      .listTables()
      .find((t) => t.name === targetTable);
    if (sqlTable === undefined) {
      console.warn(`Table ${targetTable} not found in SQL modules`);
      return;
    }

    // Create the table node with the specific table
    const tableNode = descriptor.factory(
      {
        sqlTable,
        sqlModules,
        trace: attrs.trace,
      },
      {allNodes: attrs.state.rootNodes},
    );

    // Connect table node to AddColumnsNode's secondary input (port 0)
    addConnection(tableNode, newAddColumnsNode, 0);

    // Now that rightNode is connected, set the selected column
    // (must be done after connection because onPrevNodesUpdated resets it otherwise)
    newAddColumnsNode.state.selectedColumns = [targetColumnName];

    // Update state with both new nodes
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [...currentState.rootNodes, tableNode],
      selectedNodes: new Set([newAddColumnsNode.nodeId]),
    }));
  }

  /**
   * Gets the primary input parent of a node.
   * Returns undefined for:
   * - Source nodes (no inputs)
   * - Multi-source nodes (Union, Join, IntervalIntersect - they only have secondary inputs)
   */
  private getPrimaryParent(node: QueryNode): QueryNode | undefined {
    if ('primaryInput' in node) {
      return node.primaryInput;
    }
    return undefined;
  }

  /**
   * Disconnects a node from all its parents and children.
   */
  private disconnectNodeFromGraph(node: QueryNode): void {
    // Disconnect from all parents (both primary and secondary)
    const allParents = getAllInputNodes(node);
    for (const parent of allParents) {
      removeConnection(parent, node);
    }

    // Disconnect from all children
    const children = [...node.nextNodes];
    for (const child of children) {
      removeConnection(node, child);
    }
  }

  async handleDeleteNode(attrs: ExplorePageAttrs, node: QueryNode) {
    const {state, onStateUpdate} = attrs;

    // STEP 1: Clean up resources (SQL tables, JS subscriptions, etc.)
    if (this.cleanupManager !== undefined) {
      try {
        await this.cleanupManager.cleanupNode(node);
      } catch (error) {
        // Log error but continue with deletion
        console.error('Failed to cleanup node resources:', error);
      }
    }

    // STEP 2: Capture graph structure BEFORE modification
    // We need to capture this info before removeConnection() clears the references
    const primaryParent = this.getPrimaryParent(node);
    const childConnections = captureAllChildConnections(node);
    const allInputs = getAllInputNodes(node); // Capture ALL parents (primary + secondary)

    // STEP 3: Remove the node from the graph
    this.disconnectNodeFromGraph(node);

    // STEP 4: Reconnect primary parent to children (if exists)
    // This bypasses the deleted node, maintaining data flow for PRIMARY connections only.
    //
    // IMPORTANT RULES:
    // 1. Only reconnect if deleted node fed child's PRIMARY input (portIndex === undefined)
    // 2. Secondary connections are specific to the deleted node - DROP them, don't reconnect
    // 3. Skip reconnection if parent is already connected to avoid duplicates
    // 4. Transfer deleted node's layout to docked children so they can render at same position
    const reconnectedChildren: QueryNode[] = [];
    const updatedNodeLayouts = new Map(state.nodeLayouts);
    const deletedNodeLayout = state.nodeLayouts.get(node.nodeId);

    if (primaryParent !== undefined) {
      let layoutOffsetCount = 0;
      for (const {child, portIndex} of childConnections) {
        // If deleted node fed child's secondary input, DROP the connection
        // Secondary inputs are specific to the deleted node (e.g., intervals for FilterDuring)
        if (portIndex !== undefined) {
          continue; // Don't reconnect secondary connections
        }

        // Check if parent is already connected to this child
        if (primaryParent.nextNodes.includes(child)) {
          continue; // Already connected - don't create duplicates
        }

        // Reconnect: maintain primary data flow (A → B → C becomes A → C)
        addConnection(primaryParent, child, portIndex);
        reconnectedChildren.push(child);

        // If child was docked (no layout) and deleted node had a layout,
        // transfer the layout to the child so it renders at the same position
        // For multiple children, offset their positions to avoid overlapping
        const childHasNoLayout = !state.nodeLayouts.has(child.nodeId);
        if (childHasNoLayout && deletedNodeLayout !== undefined) {
          const offsetX = layoutOffsetCount * 30; // Offset each child by 30px
          const offsetY = layoutOffsetCount * 30;
          updatedNodeLayouts.set(child.nodeId, {
            x: deletedNodeLayout.x + offsetX,
            y: deletedNodeLayout.y + offsetY,
          });
          layoutOffsetCount++;
        }
      }
    }

    // STEP 4b: Check if reconnected children can actually be rendered
    // A child becomes "unrenderable" if:
    // - It was reconnected to a parent
    // - It has no layout (was docked to deleted node)
    // - Parent has multiple children (can't render as docked anymore)
    const unrenderableChildren: QueryNode[] = [];
    if (primaryParent !== undefined && reconnectedChildren.length > 0) {
      const parentHasMultipleChildren = primaryParent.nextNodes.length > 1;
      for (const child of reconnectedChildren) {
        // Check the UPDATED layouts, not the old state
        const childHasNoLayout = !updatedNodeLayouts.has(child.nodeId);
        // If child has no layout and parent has multiple children,
        // the child can't be rendered (not as docked, not as root)
        if (childHasNoLayout && parentHasMultipleChildren) {
          unrenderableChildren.push(child);
        }
      }
    }

    // STEP 5: Update root nodes list
    // Use a Set to prevent duplicate root nodes
    const newRootNodesSet = new Set(state.rootNodes.filter((n) => n !== node));

    // Add orphaned children to root nodes so they remain visible
    // Children are orphaned ONLY if:
    // 1. There was no primary parent to reconnect them to, AND
    // 2. They were connected via PRIMARY input (not secondary)
    // Children connected via secondary input still have their own primary parent!
    if (primaryParent === undefined && childConnections.length > 0) {
      // Only children connected via primary input are truly orphaned
      const orphanedChildren = childConnections
        .filter((c) => c.portIndex === undefined) // Primary input only
        .map((c) => c.child);

      for (const child of orphanedChildren) {
        newRootNodesSet.add(child);
      }

      // Transfer deleted node's layout to orphaned children so they appear at same position
      // For multiple children, offset their positions to avoid overlapping
      if (deletedNodeLayout !== undefined) {
        let layoutOffsetCount = 0;
        for (const child of orphanedChildren) {
          const childHasNoLayout = !updatedNodeLayouts.has(child.nodeId);
          if (childHasNoLayout) {
            const offsetX = layoutOffsetCount * 30; // Offset each child by 30px
            const offsetY = layoutOffsetCount * 30;
            updatedNodeLayouts.set(child.nodeId, {
              x: deletedNodeLayout.x + offsetX,
              y: deletedNodeLayout.y + offsetY,
            });
            layoutOffsetCount++;
          }
        }
      }
    }

    // Add unrenderable children to root nodes so they become visible
    // These are children that were reconnected but can't be rendered as docked
    for (const child of unrenderableChildren) {
      newRootNodesSet.add(child);
    }

    // STEP 5b: Promote orphaned input providers to root nodes
    // Simple rule: If a node was NOT a root node, and we deleted the node that
    // consumed it, then it should become a root node.
    const orphanedInputs: QueryNode[] = [];
    for (const inputNode of allInputs) {
      // Check if this input node becomes orphaned:
      // 1. It was NOT originally a root node
      // 2. After deletion, it has no consumers (nextNodes is empty)
      const wasNotRoot = !state.rootNodes.includes(inputNode);
      const hasNoConsumers = inputNode.nextNodes.length === 0;

      if (wasNotRoot && hasNoConsumers) {
        orphanedInputs.push(inputNode);
      }
    }

    for (const inputNode of orphanedInputs) {
      newRootNodesSet.add(inputNode);
    }

    const newRootNodes = Array.from(newRootNodesSet);

    // STEP 5c: Remove the deleted node's layout from the map
    // Now that we've transferred the layout to children/orphans, clean it up
    updatedNodeLayouts.delete(node.nodeId);

    // STEP 6: Trigger validation on affected children
    // Children need to re-validate because their inputs have changed
    // (either reconnected to a different parent or lost their parent entirely)
    for (const {child} of childConnections) {
      child.onPrevNodesUpdated?.();
    }

    // Also notify orphaned input providers that their consumers changed
    for (const inputNode of orphanedInputs) {
      notifyNextNodes(inputNode);
    }

    // STEP 7: Commit state changes
    onStateUpdate((currentState) => {
      // Update selection based on current state (not stale state)
      // This is important for multi-node deletion where state changes between deletions
      const newSelectedNodes = new Set(currentState.selectedNodes);
      newSelectedNodes.delete(node.nodeId);

      return {
        ...currentState,
        rootNodes: newRootNodes,
        selectedNodes: newSelectedNodes,
        nodeLayouts: updatedNodeLayouts,
      };
    });
  }

  /**
   * Delete all currently selected nodes.
   * Batches all deletions into a single state update to create one undo point.
   */
  async handleDeleteSelectedNodes(attrs: ExplorePageAttrs): Promise<void> {
    const {state, onStateUpdate} = attrs;
    const selectedNodeIds = new Set(state.selectedNodes);

    if (selectedNodeIds.size === 0) {
      return;
    }

    // Get all nodes to delete
    const allNodes = getAllNodes(state.rootNodes);
    const nodesToDelete = allNodes.filter((n) => selectedNodeIds.has(n.nodeId));

    if (nodesToDelete.length === 0) {
      return;
    }

    // STEP 1: Clean up resources for all nodes (async operations)
    if (this.cleanupManager !== undefined) {
      for (const node of nodesToDelete) {
        try {
          await this.cleanupManager.cleanupNode(node);
        } catch (error) {
          console.error('Failed to cleanup node resources:', error);
        }
      }
    }

    // STEP 2: Capture graph info and perform all deletions in a single state update
    onStateUpdate((currentState) => {
      const nodesToDeleteSet = new Set(nodesToDelete);
      const updatedNodeLayouts = new Map(currentState.nodeLayouts);
      const newRootNodesSet = new Set(currentState.rootNodes);
      const affectedChildren: QueryNode[] = [];
      const orphanedInputs: QueryNode[] = [];

      // Process each node deletion
      for (const node of nodesToDelete) {
        // Capture info before disconnection
        const primaryParent = this.getPrimaryParent(node);
        const childConnections = captureAllChildConnections(node);
        const allInputs = getAllInputNodes(node);

        // Disconnect from graph
        this.disconnectNodeFromGraph(node);

        // Remove from root nodes
        newRootNodesSet.delete(node);

        // Remove layout
        const deletedNodeLayout = updatedNodeLayouts.get(node.nodeId);
        updatedNodeLayouts.delete(node.nodeId);

        // Reconnect primary parent to children (if parent is not also being deleted)
        if (
          primaryParent !== undefined &&
          !nodesToDeleteSet.has(primaryParent)
        ) {
          let layoutOffsetCount = 0;
          for (const {child, portIndex} of childConnections) {
            // Skip if child is also being deleted
            if (nodesToDeleteSet.has(child)) {
              continue;
            }

            // Only reconnect primary connections
            if (portIndex === undefined) {
              if (!primaryParent.nextNodes.includes(child)) {
                addConnection(primaryParent, child, portIndex);
                affectedChildren.push(child);

                // Transfer layout if child was docked
                const childHasNoLayout = !updatedNodeLayouts.has(child.nodeId);
                if (childHasNoLayout && deletedNodeLayout !== undefined) {
                  const offsetX = layoutOffsetCount * 30;
                  const offsetY = layoutOffsetCount * 30;
                  updatedNodeLayouts.set(child.nodeId, {
                    x: deletedNodeLayout.x + offsetX,
                    y: deletedNodeLayout.y + offsetY,
                  });
                  layoutOffsetCount++;
                }
              }
            }
          }
        }

        // Handle orphaned children (no parent or parent was deleted)
        if (
          primaryParent === undefined ||
          nodesToDeleteSet.has(primaryParent)
        ) {
          let layoutOffsetCount = 0;
          for (const {child, portIndex} of childConnections) {
            // Skip if child is also being deleted
            if (nodesToDeleteSet.has(child)) {
              continue;
            }

            // Only orphan primary connections
            if (portIndex === undefined) {
              newRootNodesSet.add(child);
              affectedChildren.push(child);

              // Transfer layout
              const childHasNoLayout = !updatedNodeLayouts.has(child.nodeId);
              if (childHasNoLayout && deletedNodeLayout !== undefined) {
                const offsetX = layoutOffsetCount * 30;
                const offsetY = layoutOffsetCount * 30;
                updatedNodeLayouts.set(child.nodeId, {
                  x: deletedNodeLayout.x + offsetX,
                  y: deletedNodeLayout.y + offsetY,
                });
                layoutOffsetCount++;
              }
            }
          }
        }

        // Handle orphaned input providers
        for (const inputNode of allInputs) {
          // Skip if input is also being deleted
          if (nodesToDeleteSet.has(inputNode)) {
            continue;
          }

          const wasNotRoot = !currentState.rootNodes.includes(inputNode);
          const hasNoConsumers = inputNode.nextNodes.length === 0;

          if (wasNotRoot && hasNoConsumers) {
            newRootNodesSet.add(inputNode);
            orphanedInputs.push(inputNode);
          }
        }
      }

      // Trigger validation on affected nodes
      for (const child of affectedChildren) {
        child.onPrevNodesUpdated?.();
      }
      for (const inputNode of orphanedInputs) {
        notifyNextNodes(inputNode);
      }

      // Clear selection
      return {
        ...currentState,
        rootNodes: Array.from(newRootNodesSet),
        selectedNodes: new Set<string>(),
        nodeLayouts: updatedNodeLayouts,
      };
    });
  }

  handleConnectionRemove(
    attrs: ExplorePageAttrs,
    fromNode: QueryNode,
    toNode: QueryNode,
    isSecondaryInput: boolean,
  ) {
    const {state, onStateUpdate} = attrs;

    // NOTE: The basic connection removal is already handled by graph.ts
    // This callback handles higher-level logic like reconnection and state updates

    // Only reconnect fromNode to toNode's children when removing a PRIMARY input.
    // When removing a SECONDARY input, we should NOT reconnect - the secondary
    // input node is just an auxiliary input (like intervals for FilterDuring)
    // and should not be connected to the children of the node it was feeding into.
    const shouldReconnect =
      !isSecondaryInput &&
      fromNode.nextNodes.length === 0 &&
      toNode.nextNodes.length > 0;

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

  async handleExport(state: ExplorePageState, trace: Trace) {
    const confirmed = await showExportWarning();
    if (!confirmed) return;
    exportStateAsJson(state, trace);
  }

  /**
   * Common method to load state from a JSON string.
   * Handles cleanup of existing nodes and state update.
   */
  private async loadStateFromJson(attrs: ExplorePageAttrs, json: string) {
    const {trace, sqlModulesPlugin, state, onStateUpdate} = attrs;
    const sqlModules = sqlModulesPlugin.getSqlModules();
    if (!sqlModules) {
      console.warn('Cannot load state from JSON: SQL modules not loaded yet');
      return;
    }

    await this.cleanupExistingNodes(state.rootNodes);

    const newState = deserializeState(json, trace, sqlModules);
    // Atomically update state with incremented loadGeneration
    // This ensures the Graph component sees the generation change in a single render
    onStateUpdate((currentState) => ({
      ...newState,
      loadGeneration: (currentState.loadGeneration ?? 0) + 1,
    }));
  }

  async handleImport(attrs: ExplorePageAttrs) {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.json';
    input.onchange = async (event) => {
      const files = (event.target as HTMLInputElement).files;
      if (files && files.length > 0) {
        const file = files[0];

        // Show warning modal and finalize current graph before loading
        if (!(await this.confirmAndFinalizeCurrentGraph(attrs.state))) return;

        const reader = new FileReader();
        reader.onload = async (e) => {
          const json = e.target?.result as string;
          if (!json) {
            console.error('The selected file is empty or could not be read.');
            return;
          }
          await this.loadStateFromJson(attrs, json);
        };
        reader.readAsText(file);
      }
    };
    input.click();
  }

  private handleKeyDown(event: KeyboardEvent, attrs: ExplorePageAttrs) {
    const {state} = attrs;

    // Do not interfere with text inputs
    if (
      event.target instanceof HTMLInputElement ||
      event.target instanceof HTMLTextAreaElement
    ) {
      return;
    }

    // Handle "?" to show help modal
    if (event.key === '?') {
      showDataExplorerHelp();
      event.preventDefault();
      return;
    }

    // Handle Ctrl+Enter to execute selected node
    if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
      const selectedNode = getPrimarySelectedNode(
        state.selectedNodes,
        state.rootNodes,
      );
      if (selectedNode !== undefined && this.executeFn !== undefined) {
        void this.executeFn();
        event.preventDefault();
      }
      return;
    }

    // Handle copy/paste shortcuts - these work when nodes are selected
    if ((event.ctrlKey || event.metaKey) && event.key === 'c') {
      if (state.selectedNodes.size > 0) {
        this.handleCopy(attrs);
      }
      // Always preventDefault to avoid browser copy interfering with the page,
      // even when no node is selected.
      event.preventDefault();
      return;
    }

    if ((event.ctrlKey || event.metaKey) && event.key === 'v') {
      this.handlePaste(attrs);
      event.preventDefault();
      return;
    }

    // Handle delete key - delete all selected nodes
    if (event.key === 'Delete' || event.key === 'Backspace') {
      if (state.selectedNodes.size > 0) {
        this.handleDeleteSelectedNodes(attrs);
        event.preventDefault();
      }
      return;
    }

    // For other shortcuts, skip if a node is selected to avoid interfering
    // with node-specific interactions
    if (state.selectedNodes.size > 0) {
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

  /**
   * Centralized method to load JSON from a URL path.
   * Handles confirmation, fetching, and error handling.
   */
  private async loadJsonFromPath(
    attrs: ExplorePageAttrs,
    jsonPath: string,
    errorTitle: string = 'Failed to Load',
  ): Promise<void> {
    // Show warning modal and finalize current graph before loading
    if (!(await this.confirmAndFinalizeCurrentGraph(attrs.state))) return;

    try {
      const response = await fetch(assetSrc(jsonPath));
      if (!response.ok) {
        throw new Error(
          `Failed to load: ${response.status} ${response.statusText}`,
        );
      }
      const json = await response.text();
      await this.loadStateFromJson(attrs, json);
    } catch (error) {
      console.error(`Failed to load from ${jsonPath}:`, error);
      showModal({
        title: errorTitle,
        content: () =>
          m(
            'div',
            `An error occurred while loading: ${error instanceof Error ? error.message : String(error)}`,
          ),
        buttons: [],
      });
    }
  }

  private handleUndo(attrs: ExplorePageAttrs) {
    if (!this.historyManager) {
      console.warn('Cannot undo: history manager not initialized');
      return;
    }

    const previousState = this.historyManager.undo();
    if (previousState) {
      attrs.onStateUpdate(previousState);
    }
  }

  private handleRedo(attrs: ExplorePageAttrs) {
    if (!this.historyManager) {
      console.warn('Cannot redo: history manager not initialized');
      return;
    }

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

    // Auto-initialize high-importance tables on first render when state is empty
    // Never load base JSON if we've already initialized in this session (even after clearing nodes)
    if (state.rootNodes.length === 0 && !attrs.hasAutoInitialized) {
      void this.autoInitializeHighImportanceTables(wrappedAttrs);
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
        selectedNodes: state.selectedNodes,
        nodeLayouts: state.nodeLayouts,
        labels: state.labels,
        loadGeneration: state.loadGeneration,
        isExplorerCollapsed: state.isExplorerCollapsed,
        sidebarWidth: state.sidebarWidth,
        onExecuteReady: (executeFn) => {
          this.executeFn = executeFn;
        },
        onRootNodeCreated: (node) => {
          wrappedAttrs.onStateUpdate((currentState) => ({
            ...currentState,
            rootNodes: [...currentState.rootNodes, node],
            selectedNodes: new Set([node.nodeId]),
          }));
        },
        onExplorerCollapsedChange: (collapsed) => {
          wrappedAttrs.onStateUpdate((currentState) => ({
            ...currentState,
            isExplorerCollapsed: collapsed,
          }));
        },
        onSidebarWidthChange: (width) => {
          wrappedAttrs.onStateUpdate((currentState) => ({
            ...currentState,
            sidebarWidth: width,
          }));
        },
        onNodeSelected: (node) => {
          if (node) this.selectNode(wrappedAttrs, node);
        },
        onNodeAddToSelection: (node) => {
          this.addNodeToSelection(wrappedAttrs, node);
        },
        onNodeRemoveFromSelection: (nodeId) => {
          this.removeNodeFromSelection(wrappedAttrs, nodeId);
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
        onLabelsChange: (labels) => {
          wrappedAttrs.onStateUpdate((currentState) => ({
            ...currentState,
            labels,
          }));
        },
        onAddSourceNode: (id) => {
          this.handleAddSourceNode(wrappedAttrs, id);
        },
        onAddOperationNode: (id, node) => {
          this.handleAddOperationNode(wrappedAttrs, node, id);
        },
        onClearAllNodes: () => this.handleClearAllNodes(wrappedAttrs),
        onDuplicateNode: () => {
          const selectedNode = getPrimarySelectedNode(
            state.selectedNodes,
            state.rootNodes,
          );
          if (selectedNode) {
            this.handleDuplicateNode(wrappedAttrs, selectedNode);
          }
        },
        onDeleteNode: () => {
          const selectedNode = getPrimarySelectedNode(
            state.selectedNodes,
            state.rootNodes,
          );
          if (selectedNode) {
            this.handleDeleteNode(wrappedAttrs, selectedNode);
          }
        },
        onConnectionRemove: (fromNode, toNode, isSecondaryInput) => {
          this.handleConnectionRemove(
            wrappedAttrs,
            fromNode,
            toNode,
            isSecondaryInput,
          );
        },
        onImport: () => this.handleImport(wrappedAttrs),
        onExport: () => this.handleExport(state, trace),
        onLoadEmptyTemplate: async () => {
          // Show warning modal and finalize current graph before clearing
          if (!(await this.confirmAndFinalizeCurrentGraph(state))) return;

          // Clear all nodes for empty graph and increment loadGeneration
          wrappedAttrs.onStateUpdate((currentState) => {
            return {
              ...currentState,
              rootNodes: [],
              selectedNodes: new Set(),
              nodeLayouts: new Map(),
              labels: [],
              loadGeneration: (currentState.loadGeneration ?? 0) + 1,
            };
          });
        },
        onLoadExampleByPath: (jsonPath: string) =>
          this.loadJsonFromPath(wrappedAttrs, jsonPath, 'Failed to Load'),
        onLoadExploreTemplate: async () => {
          // Show warning modal and finalize current graph before loading
          if (!(await this.confirmAndFinalizeCurrentGraph(state))) return;

          await this.createExploreGraph(wrappedAttrs);
        },
        onLoadRecentGraph: async (json: string) => {
          // Show warning modal and finalize current graph before loading
          if (!(await this.confirmAndFinalizeCurrentGraph(state))) return;
          await this.loadStateFromJson(wrappedAttrs, json);
        },
        onFilterAdd: (node, filter, filterOperator) => {
          this.handleFilterAdd(wrappedAttrs, node, filter, filterOperator);
        },
        onColumnAdd: (node, column) => {
          this.handleColumnAdd(wrappedAttrs, node, column);
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
