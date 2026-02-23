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
import {QueryNode} from './query_node';
import {ensureAllNodeActions} from './node_actions';
import {Trace} from '../../public/trace';

import {
  confirmAndFinalizeCurrentGraph,
  exportGraph,
  importGraph,
  loadGraphFromJson,
  loadGraphFromPath,
  initializeHighImportanceTables,
  createExploreGraph,
  GraphIODeps,
} from './graph_io';
import {registerCoreNodes} from './query_builder/core_nodes';
import {nodeRegistry} from './query_builder/node_registry';
import {QueryExecutionService} from './query_builder/query_execution_service';
import {CleanupManager} from './query_builder/cleanup_manager';
import {HistoryManager} from './history_manager';
import {getPrimarySelectedNode} from './selection_utils';
import {getAllNodes} from './query_builder/graph_utils';
import {
  cleanupExistingNodes,
  addOperationNode,
  addSourceNode,
  addAndConnectTable,
  insertNodeAtPort,
  clearAllNodes,
  duplicateNode,
  deleteNode,
  deleteSelectedNodes,
  removeNodeConnection,
} from './node_crud_operations';
import type {NodeCrudDeps} from './node_crud_operations';
import {addFilter, addColumnFromJoinid} from './datagrid_node_creation';
import {showDataExplorerHelp} from './data_explorer_help_modal';

import type {ClipboardEntry, ClipboardConnection} from './clipboard_operations';
import {copySelectedNodes, pasteClipboardNodes} from './clipboard_operations';

registerCoreNodes();

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

  private handleCopy(attrs: ExplorePageAttrs): void {
    const result = copySelectedNodes(attrs.state);
    if (result !== undefined) {
      attrs.onStateUpdate((currentState) => ({
        ...currentState,
        ...result,
      }));
    }
  }

  private handlePaste(attrs: ExplorePageAttrs): void {
    attrs.onStateUpdate((currentState) => {
      const result = pasteClipboardNodes(currentState);
      if (result === undefined) return currentState;
      return {...currentState, ...result};
    });
  }

  private handleKeyDown(
    event: KeyboardEvent,
    attrs: ExplorePageAttrs,
    nodeCrudDeps: NodeCrudDeps,
    graphIODeps: GraphIODeps,
  ) {
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
        deleteSelectedNodes(nodeCrudDeps, attrs.state);
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
        addSourceNode(nodeCrudDeps, attrs.state, id);
        event.preventDefault(); // Prevent default browser actions for this key
        return;
      }
    }

    // Handle other shortcuts
    switch (event.key) {
      case 'i':
        importGraph(graphIODeps, attrs.state);
        break;
      case 'e':
        exportGraph(attrs.state, attrs.trace);
        break;
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

    // Initialize services if not already done
    if (this.queryExecutionService === undefined) {
      this.queryExecutionService = new QueryExecutionService(
        attrs.trace.engine,
      );
      this.cleanupManager = new CleanupManager(this.queryExecutionService);
    }

    // Construct deps objects once per render cycle.
    // nodeActionHandlers closures reference nodeCrudDeps lazily — they are
    // only invoked on user interaction, well after the const is initialized.
    const nodeCrudDeps: NodeCrudDeps = {
      trace: attrs.trace,
      sqlModules,
      onStateUpdate: wrappedOnStateUpdate,
      cleanupManager: this.cleanupManager,
      initializedNodes: this.initializedNodes,
      nodeActionHandlers: {
        onAddAndConnectTable: (
          tableName: string,
          node: QueryNode,
          portIndex: number,
        ) => {
          addAndConnectTable(
            nodeCrudDeps,
            wrappedAttrs.state,
            tableName,
            node,
            portIndex,
          );
        },
        onInsertNodeAtPort: (
          node: QueryNode,
          portIndex: number,
          descriptorKey: string,
        ) => {
          insertNodeAtPort(
            nodeCrudDeps,
            wrappedAttrs.state,
            node,
            portIndex,
            descriptorKey,
          );
        },
      },
    };

    const graphIODeps: GraphIODeps = {
      trace: attrs.trace,
      sqlModules,
      onStateUpdate: wrappedOnStateUpdate,
      cleanupExistingNodes: (rootNodes) =>
        cleanupExistingNodes(
          this.cleanupManager,
          this.initializedNodes,
          rootNodes,
        ),
    };

    // Ensure all nodes have actions initialized (e.g., nodes from imported state)
    // This is efficient - only processes nodes not yet initialized
    const allNodes = getAllNodes(state.rootNodes);
    ensureAllNodeActions(
      allNodes,
      this.initializedNodes,
      nodeCrudDeps.nodeActionHandlers,
    );

    // Auto-initialize high-importance tables on first render when state is empty
    // Never load base JSON if we've already initialized in this session (even after clearing nodes)
    if (state.rootNodes.length === 0 && !attrs.hasAutoInitialized) {
      void initializeHighImportanceTables(
        graphIODeps,
        attrs.setHasAutoInitialized,
      );
    }

    return m(
      '.pf-explore-page',
      {
        onkeydown: (e: KeyboardEvent) =>
          this.handleKeyDown(e, wrappedAttrs, nodeCrudDeps, graphIODeps),
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
        graphCallbacks: {
          onNodeSelected: (node) => {
            this.selectNode(wrappedAttrs, node);
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
            addSourceNode(nodeCrudDeps, wrappedAttrs.state, id);
          },
          onAddOperationNode: (id, node) => {
            addOperationNode(nodeCrudDeps, wrappedAttrs.state, node, id);
          },
          onClearAllNodes: () =>
            clearAllNodes(nodeCrudDeps, wrappedAttrs.state),
          onDuplicateNode: (node) => {
            duplicateNode(wrappedAttrs.onStateUpdate, node);
          },
          onDeleteNode: (node) => {
            deleteNode(nodeCrudDeps, wrappedAttrs.state, node);
          },
          onConnectionRemove: (fromNode, toNode, isSecondaryInput) => {
            removeNodeConnection(
              wrappedAttrs.state,
              wrappedAttrs.onStateUpdate,
              fromNode,
              toNode,
              isSecondaryInput,
            );
          },
          onImport: () => importGraph(graphIODeps, state),
          onExport: () => exportGraph(state, trace),
        },
        onLoadEmptyTemplate: async () => {
          if (!(await confirmAndFinalizeCurrentGraph(state))) return;

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
          loadGraphFromPath(graphIODeps, state, jsonPath, 'Failed to Load'),
        onLoadExploreTemplate: async () => {
          if (!(await confirmAndFinalizeCurrentGraph(state))) return;
          await createExploreGraph(graphIODeps);
        },
        onLoadRecentGraph: async (json: string) => {
          if (!(await confirmAndFinalizeCurrentGraph(state))) return;
          await loadGraphFromJson(graphIODeps, state.rootNodes, json);
        },
        onFilterAdd: (node, filter, filterOperator) => {
          addFilter(nodeCrudDeps, node, filter, filterOperator);
        },
        onColumnAdd: (node, column) => {
          addColumnFromJoinid(nodeCrudDeps, wrappedAttrs.state, node, column);
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
