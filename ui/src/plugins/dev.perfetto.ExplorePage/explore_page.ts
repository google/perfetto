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
} from './query_node';
import {UIFilter} from './query_builder/operations/filter';
import {Trace} from '../../public/trace';

import {exportStateAsJson, importStateFromJson} from './json_handler';
import {showImportWithStatementModal} from './sql_json_handler';
import {registerCoreNodes} from './query_builder/core_nodes';
import {nodeRegistry} from './query_builder/node_registry';
import {MaterializationService} from './query_builder/materialization_service';
import {HistoryManager} from './history_manager';

registerCoreNodes();

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
  private materializationService?: MaterializationService;
  private historyManager?: HistoryManager;
  private initializedNodes = new Set<string>();

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
      let initialState: Partial<QueryNodeState> | null = {};
      if (descriptor.preCreate) {
        const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
        if (!sqlModules) return;
        initialState = await descriptor.preCreate({sqlModules});
      }

      if (initialState === null) {
        return;
      }

      const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
      if (!sqlModules) return;

      // Use a wrapper object to hold the node reference (allows mutation without 'let')
      const nodeRef: {current?: QueryNode} = {};

      const isMultisource = descriptor.type === 'multisource';

      const nodeState: QueryNodeState = {
        ...initialState,
        // For modification nodes, set prevNode; multisource nodes will be connected via addConnection
        ...(isMultisource ? {} : {prevNode: node}),
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

      if (isMultisource) {
        // For multisource nodes: just connect and add to root nodes
        // Don't insert in-between - the node combines multiple sources
        addConnection(node, newNode);

        onStateUpdate((currentState) => ({
          ...currentState,
          rootNodes: [...currentState.rootNodes, newNode],
          selectedNode: newNode,
        }));
      } else {
        // For modification nodes: insert between the target and its children
        // Store the existing next nodes
        const existingNextNodes = [...node.nextNodes];

        // Clear the node's next nodes (we'll reconnect through the new node)
        node.nextNodes = [];

        // Connect: node -> newNode
        addConnection(node, newNode);

        // Connect: newNode -> each existing next node
        for (const nextNode of existingNextNodes) {
          if (nextNode !== undefined) {
            // First remove the old connection from node to nextNode (if it still exists)
            removeConnection(node, nextNode);
            // Then add connection from newNode to nextNode
            addConnection(newNode, nextNode);
          }
        }

        onStateUpdate((currentState) => ({
          ...currentState,
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

    let initialState: Partial<QueryNodeState> | null = {};

    if (descriptor.preCreate) {
      const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
      if (!sqlModules) return;
      initialState = await descriptor.preCreate({sqlModules});
    }

    if (initialState === null) {
      return;
    }

    const newNode = descriptor.factory(
      {
        ...initialState,
        trace: attrs.trace,
      },
      {allNodes: attrs.state.rootNodes},
    );

    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [...currentState.rootNodes, newNode],
      selectedNode: newNode,
    }));
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
    let inputNode: QueryNode | undefined;
    if ('inputNodes' in targetNode && targetNode.inputNodes) {
      inputNode = targetNode.inputNodes[portIndex];
    } else if (
      'prevNodes' in targetNode &&
      Array.isArray(targetNode.prevNodes)
    ) {
      inputNode = targetNode.prevNodes[portIndex];
    }

    if (!inputNode) {
      console.warn(`No input node found at port ${portIndex}`);
      return;
    }

    // Create the ModifyColumns node with the input node as prevNode
    const newNode = descriptor.factory(
      {
        prevNode: inputNode,
        sqlModules,
        trace: attrs.trace,
      },
      {allNodes: attrs.state.rootNodes},
    );

    // Remove the old connection from inputNode to targetNode
    removeConnection(inputNode, targetNode);

    // Add connection from inputNode to ModifyColumns node
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
    // Clean up materialized tables for all nodes
    if (this.materializationService !== undefined) {
      const allNodes = this.getAllNodes(attrs.state.rootNodes);
      const materialized = allNodes.filter(
        (node) => node.state.materialized === true,
      );

      // Drop all materializations in parallel
      const results = await Promise.allSettled(
        materialized.map((node) =>
          this.materializationService!.dropMaterialization(node),
        ),
      );

      // Log any failures but don't block the clear operation
      results.forEach((result, index) => {
        if (result.status === 'rejected') {
          console.error(
            `Failed to drop materialization for node ${materialized[index].nodeId}:`,
            result.reason,
          );
        }
      });
    }

    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [],
      selectedNode: undefined,
    }));
  }

  private getAllNodes(rootNodes: QueryNode[]): QueryNode[] {
    const allNodes: QueryNode[] = [];
    const visited = new Set<string>();
    const queue = [...rootNodes];

    while (queue.length > 0) {
      const node = queue.shift()!;
      if (visited.has(node.nodeId)) {
        continue;
      }
      visited.add(node.nodeId);
      allNodes.push(node);

      // Traverse forward edges
      queue.push(...node.nextNodes);

      // Traverse backward edges
      if ('prevNode' in node && node.prevNode) {
        queue.push(node.prevNode);
      } else if ('prevNodes' in node) {
        for (const prevNode of node.prevNodes) {
          if (prevNode !== undefined) {
            queue.push(prevNode);
          }
        }
      }
    }

    return allNodes;
  }

  handleDuplicateNode(attrs: ExplorePageAttrs, node: QueryNode) {
    const {onStateUpdate} = attrs;
    onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [...currentState.rootNodes, node.clone()],
    }));
  }

  async handleFilterAdd(
    attrs: ExplorePageAttrs,
    sourceNode: QueryNode,
    filter: {column: string; op: string; value?: unknown},
  ) {
    // If the source node is already a FilterNode, just add the filter to it
    if (sourceNode.type === NodeType.kFilter) {
      sourceNode.state.filters = [
        ...(sourceNode.state.filters ?? []),
        filter as UIFilter,
      ];
      attrs.onStateUpdate((currentState) => ({...currentState}));
      return;
    }

    // If the source node has exactly one child and it's a FilterNode, add to that
    if (
      sourceNode.nextNodes.length === 1 &&
      sourceNode.nextNodes[0].type === NodeType.kFilter
    ) {
      const existingFilterNode = sourceNode.nextNodes[0];
      existingFilterNode.state.filters = [
        ...(existingFilterNode.state.filters ?? []),
        filter as UIFilter,
      ];
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

    // Add the filter to the newly created FilterNode
    if (newFilterNode) {
      newFilterNode.state.filters = [filter as UIFilter];
      attrs.onStateUpdate((currentState) => ({
        ...currentState,
        selectedNode: newFilterNode,
      }));
    }
  }

  async handleDeleteNode(attrs: ExplorePageAttrs, node: QueryNode) {
    const {state, onStateUpdate} = attrs;

    // Clean up materialized table if it exists
    if (
      this.materializationService !== undefined &&
      node.state.materialized === true
    ) {
      try {
        await this.materializationService.dropMaterialization(node);
      } catch (e) {
        console.error(
          `Failed to drop materialization for node ${node.nodeId}:`,
          e,
        );
        // Continue with node deletion even if materialization cleanup fails
      }
    }

    let newRootNodes = state.rootNodes.filter((n) => n !== node);
    if (state.rootNodes.includes(node) && node.nextNodes.length > 0) {
      newRootNodes = [...newRootNodes, ...node.nextNodes];
    }

    // Get parent nodes before removing connections
    const parentNodes: QueryNode[] = [];
    if ('prevNode' in node && node.prevNode) {
      parentNodes.push(node.prevNode);
    } else if ('prevNodes' in node) {
      for (const prevNode of node.prevNodes) {
        if (prevNode !== undefined) parentNodes.push(prevNode);
      }
    }

    // Also collect nodes from inputNodes (side ports)
    if ('inputNodes' in node && node.inputNodes) {
      for (const inputNode of node.inputNodes) {
        if (inputNode !== undefined) parentNodes.push(inputNode);
      }
    }

    // Get child nodes
    const childNodes = [...node.nextNodes];

    // Remove all connections to/from the deleted node
    for (const parent of parentNodes) {
      removeConnection(parent, node);
    }
    for (const child of childNodes) {
      removeConnection(node, child);
    }

    // Reconnect parents to children (bypass the deleted node)
    for (const parent of parentNodes) {
      for (const child of childNodes) {
        addConnection(parent, child);
      }
    }

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
    if ('prevNode' in toNode && toNode.prevNode === undefined) {
      // toNode is a ModificationNode that's now orphaned
      // Add it to rootNodes so it remains visible (but invalid)
      const newRootNodes = state.rootNodes.includes(toNode)
        ? state.rootNodes
        : [...state.rootNodes, toNode];

      onStateUpdate((currentState) => ({
        ...currentState,
        rootNodes: newRootNodes,
      }));
    } else if ('prevNodes' in toNode) {
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
    const allNodes = this.getAllNodes(state.rootNodes);
    for (const node of allNodes) {
      this.ensureNodeActions(wrappedAttrs, node);
    }

    return m(
      '.pf-explore-page',
      {
        onkeydown: (e: KeyboardEvent) => this.handleKeyDown(e, wrappedAttrs),
        oncreate: (vnode) => {
          // Initialize materialization service
          if (this.materializationService === undefined) {
            this.materializationService = new MaterializationService(
              attrs.trace.engine,
            );
          }
          (vnode.dom as HTMLElement).focus();
        },
        tabindex: 0,
      },
      m(Builder, {
        trace,
        sqlModules,
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
        onFilterAdd: (node, filter) => {
          this.handleFilterAdd(wrappedAttrs, node, filter);
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
