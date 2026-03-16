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
import {getOrCreate} from '../../base/utils';
import {shortUuid} from '../../base/uuid';
import {MenuItem} from '../../widgets/menu';
import {Tabs, TabsTab} from '../../widgets/tabs';
import {Button, ButtonBar} from '../../widgets/button';
import {serializeState, deserializeState} from './json_handler';

import {
  confirmAndFinalizeCurrentGraph,
  exportGraph,
  importGraph,
  loadGraphFromJson,
  loadGraphFromPath,
  createDataExplorerGraph,
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
import {Dashboard} from './dashboard/dashboard';
import {isDashboardNode} from './query_builder/nodes/dashboard_node';
import {
  dashboardRegistry,
  DashboardBrushFilter,
  DashboardItem,
} from './dashboard/dashboard_registry';
import type {NodeCrudDeps} from './node_crud_operations';
import {addFilter, addColumnFromJoinid} from './datagrid_node_creation';
import {showHelp} from './help_modal';

import {copySelectedNodes, pasteClipboardNodes} from './clipboard_operations';
import type {ClipboardResult} from './clipboard_operations';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';

registerCoreNodes();

/** State for a single dashboard within a graph tab. */
export interface DashboardTabState {
  readonly id: string;
  title: string;
  items: DashboardItem[];
  brushFilters: Map<string, DashboardBrushFilter[]>;
}

export interface DataExplorerTab {
  readonly id: string;
  title: string;
  state: DataExplorerState;
  // Each graph tab has one or more dashboards.
  dashboards: DashboardTabState[];
  // Which sub-tab is active: 'graph' (default) or a dashboard ID.
  activeSubTab?: string;
}

export interface DataExplorerState {
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
}

type StateUpdateFn = (
  update:
    | DataExplorerState
    | ((current: DataExplorerState) => DataExplorerState),
) => void;

interface DataExplorerAttrs {
  readonly trace: Trace;
  readonly sqlModulesPlugin: SqlModulesPlugin;
  // Active tab's state (convenience reference)
  readonly state: DataExplorerState;
  // State updater for the active tab (used by keyboard handlers, etc.)
  readonly onStateUpdate: StateUpdateFn;
  // Factory that returns a per-tab state update function.
  // Used in renderTabContent to create tab-scoped updaters that remain
  // correct even if the active tab changes while async work is in flight.
  readonly makeOnStateUpdate: (tabId: string) => StateUpdateFn;
  // Multi-tab props
  readonly tabs: DataExplorerTab[];
  readonly activeTabId: string;
  readonly onTabAdd: () => void;
  readonly onTabClose: (tabId: string) => void;
  readonly onTabChange: (tabId: string) => void;
  readonly onTabRename: (tabId: string, newName: string) => void;
  readonly onTabReorder: (
    draggedId: string,
    beforeId: string | undefined,
  ) => void;
  // Creates a new tab with the given title and state, inserted after the
  // tab identified by afterTabId.
  readonly onTabAddWithState: (
    title: string,
    state: DataExplorerState,
    afterTabId: string,
  ) => void;
  // Notify the plugin that dashboard-local state changed (items/filters)
  // so it can trigger debounced saves.
  readonly onDashboardStateChange: () => void;
}

// Per-tab service instances that live for the lifetime of the tab.
interface TabServices {
  queryExecutionService: QueryExecutionService;
  cleanupManager: CleanupManager;
  historyManager?: HistoryManager;
  initializedNodes: Set<string>;
  executeFn?: () => Promise<void>;
}

export class DataExplorer implements m.ClassComponent<DataExplorerAttrs> {
  // Shared clipboard across all tabs (not persisted).
  private clipboard?: ClipboardResult;

  // Per-tab services, keyed by tab ID.
  private tabServices = new Map<string, TabServices>();

  private getOrCreateServices(
    tabId: string,
    engine: Trace['engine'],
  ): TabServices {
    return getOrCreate(this.tabServices, tabId, () => {
      const qes = new QueryExecutionService(engine);
      return {
        queryExecutionService: qes,
        cleanupManager: new CleanupManager(qes),
        initializedNodes: new Set<string>(),
      };
    });
  }

  private getActiveServices(activeTabId: string): TabServices | undefined {
    return this.tabServices.get(activeTabId);
  }

  private selectNode(attrs: DataExplorerAttrs, node: QueryNode) {
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      selectedNodes: new Set([node.nodeId]),
    }));
  }

  private addNodeToSelection(attrs: DataExplorerAttrs, node: QueryNode) {
    attrs.onStateUpdate((currentState) => {
      const newSelectedNodes = new Set(currentState.selectedNodes);
      newSelectedNodes.add(node.nodeId);
      return {
        ...currentState,
        selectedNodes: newSelectedNodes,
      };
    });
  }

  private removeNodeFromSelection(attrs: DataExplorerAttrs, nodeId: string) {
    attrs.onStateUpdate((currentState) => {
      const newSelectedNodes = new Set(currentState.selectedNodes);
      newSelectedNodes.delete(nodeId);
      return {
        ...currentState,
        selectedNodes: newSelectedNodes,
      };
    });
  }

  private deselectNode(attrs: DataExplorerAttrs) {
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      selectedNodes: new Set(),
    }));
  }

  private handleCopy(attrs: DataExplorerAttrs): void {
    const result = copySelectedNodes(attrs.state);
    if (result !== undefined) {
      this.clipboard = result;
    }
  }

  private handlePaste(attrs: DataExplorerAttrs): void {
    if (this.clipboard === undefined) return;
    const clipboard = this.clipboard;
    attrs.onStateUpdate((currentState) => {
      const result = pasteClipboardNodes(currentState, clipboard);
      if (result === undefined) return currentState;
      return {...currentState, ...result};
    });
  }

  private handleKeyDown(
    event: KeyboardEvent,
    attrs: DataExplorerAttrs,
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
      showHelp();
      event.preventDefault();
      return;
    }

    const activeServices = this.getActiveServices(attrs.activeTabId);

    // Handle Ctrl+Enter to execute selected node
    if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
      const selectedNode = getPrimarySelectedNode(
        state.selectedNodes,
        state.rootNodes,
      );
      if (
        selectedNode !== undefined &&
        activeServices?.executeFn !== undefined
      ) {
        void activeServices.executeFn();
        event.preventDefault();
      }
      return;
    }

    // Handle copy shortcut - text selection takes priority over node copy,
    // so users can copy proto/SQL from the sidebar, error messages, etc.
    if ((event.ctrlKey || event.metaKey) && event.key === 'c') {
      const textSelection = window.getSelection();
      const hasTextSelected =
        textSelection !== null && textSelection.toString().length > 0;
      if (!hasTextSelected && state.selectedNodes.size > 0) {
        this.handleCopy(attrs);
        event.preventDefault();
      }
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
        importGraph(graphIODeps, (title, newState) =>
          attrs.onTabAddWithState(title, newState, attrs.activeTabId),
        );
        break;
      case 'e':
        exportGraph(state, attrs.trace);
        break;
    }
  }

  private handleUndo(attrs: DataExplorerAttrs) {
    const historyManager = this.getActiveServices(
      attrs.activeTabId,
    )?.historyManager;
    if (!historyManager) {
      console.warn('Cannot undo: history manager not initialized');
      return;
    }

    const previousState = historyManager.undo();
    if (previousState) {
      attrs.onStateUpdate(previousState);
    }
  }

  private handleRedo(attrs: DataExplorerAttrs) {
    const historyManager = this.getActiveServices(
      attrs.activeTabId,
    )?.historyManager;
    if (!historyManager) {
      console.warn('Cannot redo: history manager not initialized');
      return;
    }

    const nextState = historyManager.redo();
    if (nextState) {
      attrs.onStateUpdate(nextState);
    }
  }

  // Render the content for a single tab. This includes the Builder and all
  // per-tab service setup (history, query execution, cleanup, etc.).
  private renderTabContent(
    attrs: DataExplorerAttrs,
    tab: DataExplorerTab,
    sqlModules: SqlModules,
  ): m.Children {
    const {trace} = attrs;
    const {state} = tab;
    const isActive = tab.id === attrs.activeTabId;

    const services = this.getOrCreateServices(tab.id, trace.engine);

    // Initialize history manager for this tab if not already done
    if (services.historyManager === undefined) {
      services.historyManager = new HistoryManager(trace, sqlModules);
      services.historyManager.pushState(state);
    }

    // Create a per-tab state updater that wraps history tracking.
    // Uses makeOnStateUpdate(tab.id) so the updater is always bound to THIS
    // tab's state, even if the active tab changes while async work is in flight.
    const tabOnStateUpdate = attrs.makeOnStateUpdate(tab.id);
    const wrappedOnStateUpdate: StateUpdateFn = (update) => {
      tabOnStateUpdate((currentState) => {
        const newState =
          typeof update === 'function' ? update(currentState) : update;
        services.historyManager?.pushState(newState);
        return newState;
      });
    };

    const wrappedAttrs = {
      ...attrs,
      state,
      onStateUpdate: wrappedOnStateUpdate,
    };

    // Construct deps objects once per render cycle.
    const nodeCrudDeps: NodeCrudDeps = {
      trace,
      sqlModules,
      onStateUpdate: wrappedOnStateUpdate,
      cleanupManager: services.cleanupManager,
      initializedNodes: services.initializedNodes,
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
      trace,
      sqlModules,
      onStateUpdate: wrappedOnStateUpdate,
      cleanupExistingNodes: (rootNodes) =>
        cleanupExistingNodes(
          services.cleanupManager,
          services.initializedNodes,
          rootNodes,
        ),
    };

    const allNodes = getAllNodes(state.rootNodes);

    // Set callbacks on dashboard nodes so they can resolve table names and
    // trigger execution even when the graph tab is not active.
    for (const node of allNodes) {
      if (!isDashboardNode(node)) continue;
      // Propagate the stable tab ID so sources are namespaced by graph.
      // Re-publish if the graphId changed so the registry has the right key.
      const prevGraphId = node.state.graphId;
      node.state.graphId = tab.id;
      if (prevGraphId !== tab.id) {
        node.onPrevNodesUpdated?.();
      }
      if (node.state.getTableNameForNode === undefined) {
        node.state.getTableNameForNode = (nodeId: string) =>
          services.queryExecutionService.getTableName(nodeId);
      }
      if (node.state.requestNodeExecution === undefined) {
        node.state.requestNodeExecution = (nodeId: string) => {
          const targetNode = allNodes.find((n) => n.nodeId === nodeId);
          if (targetNode === undefined) return Promise.resolve();
          return services.queryExecutionService
            .processNode(targetNode, trace.engine, allNodes, {manual: true})
            .then(() => {});
        };
      }
    }

    // Only do full node processing for the active tab to avoid unnecessary work
    if (isActive) {
      // Provide getTableNameForNode callback for all nodes (used by
      // add_columns_node and others).
      for (const node of allNodes) {
        if (node.state.getTableNameForNode === undefined) {
          node.state.getTableNameForNode = (nodeId: string) =>
            services.queryExecutionService.getTableName(nodeId);
        }
      }

      // Ensure all nodes have actions initialized
      ensureAllNodeActions(
        allNodes,
        services.initializedNodes,
        nodeCrudDeps.nodeActionHandlers,
      );

      // Store deps for keyboard handler access
      this.activeNodeCrudDeps = nodeCrudDeps;
      this.activeGraphIODeps = graphIODeps;
    }

    // Sized wrapper so DrawerPanel can read a non-zero clientHeight;
    // Gate (display:contents) elements have clientHeight === 0.
    return m(
      '.pf-data-explorer__tab-content',
      m(Builder, {
        trace,
        sqlModules,
        queryExecutionService: services.queryExecutionService,
        rootNodes: state.rootNodes,
        selectedNodes: state.selectedNodes,
        nodeLayouts: state.nodeLayouts,
        labels: state.labels,
        loadGeneration: state.loadGeneration,
        isExplorerCollapsed: state.isExplorerCollapsed,
        sidebarWidth: state.sidebarWidth,
        onExecuteReady: (executeFn) => {
          services.executeFn = executeFn;
        },
        onRootNodeCreated: (node) => {
          wrappedOnStateUpdate((currentState) => ({
            ...currentState,
            rootNodes: [...currentState.rootNodes, node],
            selectedNodes: new Set([node.nodeId]),
          }));
        },
        onExplorerCollapsedChange: (collapsed) => {
          wrappedOnStateUpdate((currentState) => ({
            ...currentState,
            isExplorerCollapsed: collapsed,
          }));
        },
        onSidebarWidthChange: (width) => {
          wrappedOnStateUpdate((currentState) => ({
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
            wrappedOnStateUpdate((currentState) => {
              const newNodeLayouts = new Map(currentState.nodeLayouts);
              newNodeLayouts.set(nodeId, layout);
              return {
                ...currentState,
                nodeLayouts: newNodeLayouts,
              };
            });
          },
          onLabelsChange: (labels) => {
            wrappedOnStateUpdate((currentState) => ({
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
            duplicateNode(wrappedOnStateUpdate, node);
          },
          onDeleteNode: (node) => {
            deleteNode(nodeCrudDeps, wrappedAttrs.state, node);
          },
          onConnectionRemove: (fromNode, toNode, isSecondaryInput) => {
            removeNodeConnection(
              wrappedAttrs.state,
              wrappedOnStateUpdate,
              fromNode,
              toNode,
              isSecondaryInput,
            );
          },
          onImport: () =>
            importGraph(graphIODeps, (title, newState) =>
              attrs.onTabAddWithState(title, newState, tab.id),
            ),
          onExport: () => exportGraph(state, trace),
        },
        onLoadEmptyTemplate: async () => {
          if (!(await confirmAndFinalizeCurrentGraph(state))) return;

          wrappedOnStateUpdate((currentState) => {
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
        onLoadDataExplorerTemplate: async () => {
          if (!(await confirmAndFinalizeCurrentGraph(state))) return;
          await createDataExplorerGraph(graphIODeps);
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
          wrappedOnStateUpdate((currentState) => {
            return {...currentState};
          });
        },
        onUndo: () => this.handleUndo(attrs),
        onRedo: () => this.handleRedo(attrs),
        canUndo: services.historyManager?.canUndo() ?? false,
        canRedo: services.historyManager?.canRedo() ?? false,
      }),
    );
  }

  // Stored references for the active tab's deps, used by keyboard handler.
  private activeNodeCrudDeps?: NodeCrudDeps;
  private activeGraphIODeps?: GraphIODeps;

  /** Get exported sources for a specific graph tab. */
  private getDashboardSources(graphTabId: string) {
    return dashboardRegistry.getExportedSourcesForGraph(graphTabId);
  }

  /** Trigger debounced saves after dashboard state changes. */
  private triggerSave(attrs: DataExplorerAttrs) {
    attrs.onDashboardStateChange();
    m.redraw();
  }

  private addDashboard(tab: DataExplorerTab, attrs: DataExplorerAttrs): void {
    const existingNames = new Set(tab.dashboards.map((d) => d.title));
    let num = 1;
    while (existingNames.has(`Dashboard ${num}`)) {
      num++;
    }
    const db: DashboardTabState = {
      id: shortUuid(),
      title: `Dashboard ${num}`,
      items: [],
      brushFilters: new Map(),
    };
    tab.dashboards.push(db);
    tab.activeSubTab = db.id;
    this.triggerSave(attrs);
  }

  /**
   * Render a graph tab with nested sub-tabs (Graph | Dashboard 1 | ... | +).
   * The graph builder is always initialized (for dashboard node execution),
   * but we conditionally show either the builder or the dashboard view.
   */
  private renderTabWithSubTabs(
    attrs: DataExplorerAttrs,
    tab: DataExplorerTab,
    sqlModules: SqlModules,
  ): m.Children {
    const activeSubTab = tab.activeSubTab ?? 'graph';
    const graphContent = this.renderTabContent(attrs, tab, sqlModules);

    const subTabBar = m(
      '.pf-data-explorer__sub-tab-bar',
      m(
        ButtonBar,
        m(Button, {
          label: 'Graph',
          icon: 'account_tree',
          active: activeSubTab === 'graph',
          onclick: () => {
            tab.activeSubTab = 'graph';
            m.redraw();
          },
        }),
        ...tab.dashboards.map((db) =>
          m(Button, {
            label: db.title,
            icon: 'dashboard',
            active: activeSubTab === db.id,
            onclick: () => {
              tab.activeSubTab = db.id;
              m.redraw();
            },
          }),
        ),
        m(Button, {
          icon: 'add',
          title: 'Add dashboard',
          onclick: () => this.addDashboard(tab, attrs),
        }),
      ),
    );

    // Find active dashboard if any.
    const activeDashboard = tab.dashboards.find((db) => db.id === activeSubTab);

    if (activeDashboard !== undefined) {
      return m('.pf-data-explorer__tab-with-subtabs', [
        subTabBar,
        m(
          '.pf-data-explorer__tab-content',
          m(Dashboard, {
            dashboardId: activeDashboard.id,
            trace: attrs.trace,
            items: activeDashboard.items,
            sources: this.getDashboardSources(tab.id),
            brushFilters: activeDashboard.brushFilters,
            onItemsChange: (items) => {
              activeDashboard.items = items;
              this.triggerSave(attrs);
            },
            onBrushFiltersChange: (filters) => {
              activeDashboard.brushFilters = filters;
              this.triggerSave(attrs);
            },
          }),
        ),
      ]);
    }

    return m('.pf-data-explorer__tab-with-subtabs', [subTabBar, graphContent]);
  }

  view({attrs}: m.CVnode<DataExplorerAttrs>) {
    const {tabs, activeTabId} = attrs;

    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();

    if (!sqlModules) {
      return m(
        '.pf-data-explorer',
        m(
          '.pf-data-explorer__header',
          m('h1', 'Loading SQL Modules, please wait...'),
        ),
      );
    }

    // Build tab entries for the Tabs widget
    const tabEntries: TabsTab[] = tabs.map((tab) => ({
      key: tab.id,
      title: tab.title,
      leftIcon: 'account_tree',
      closeButton: tabs.length > 1,
      content: this.renderTabWithSubTabs(attrs, tab, sqlModules),
      menuItems: m(MenuItem, {
        label: 'Duplicate tab',
        icon: 'content_copy',
        onclick: () => {
          const sqlMods = attrs.sqlModulesPlugin.getSqlModules();
          if (sqlMods === undefined) return;
          const json = serializeState(tab.state);
          const clonedState = deserializeState(json, attrs.trace, sqlMods);
          attrs.onTabAddWithState(`${tab.title} (copy)`, clonedState, tab.id);
        },
      }),
    }));

    return m(
      '.pf-data-explorer',
      {
        onkeydown: (e: KeyboardEvent) => {
          if (this.activeNodeCrudDeps && this.activeGraphIODeps) {
            this.handleKeyDown(
              e,
              attrs,
              this.activeNodeCrudDeps,
              this.activeGraphIODeps,
            );
          }
        },
        oncreate: (vnode) => {
          (vnode.dom as HTMLElement).focus();
        },
        onremove: () => {
          // Clean up all materialized tables for all tabs in parallel
          void Promise.all(
            [...this.tabServices].map(([tabId, services]) => {
              const tab = tabs.find((t) => t.id === tabId);
              const rootNodes = tab ? getAllNodes(tab.state.rootNodes) : [];
              return services.cleanupManager
                .cleanupAll(rootNodes)
                .catch((e) => console.warn(`Tab ${tabId} cleanup failed:`, e));
            }),
          ).finally(() => this.tabServices.clear());
        },
        tabindex: 0,
      },
      m(Tabs, {
        className: 'pf-data-explorer__tabs',
        tabs: tabEntries,
        activeTabKey: activeTabId,
        reorderable: true,
        newTabContent: m(Button, {
          icon: 'add',
          className: 'pf-tabs__new-tab-btn',
          onclick: () => attrs.onTabAdd(),
        }),
        onTabChange: (key) => attrs.onTabChange(key),
        onTabRename: (key, newTitle) => {
          attrs.onTabRename(key, newTitle);
        },
        onTabClose: (key) => {
          // Clean up services for the closed tab eagerly
          const services = this.tabServices.get(key);
          if (services) {
            const tab = tabs.find((t) => t.id === key);
            const rootNodes = tab ? getAllNodes(tab.state.rootNodes) : [];
            void services.cleanupManager
              .cleanupAll(rootNodes)
              .catch((e) => console.warn(`Tab ${key} cleanup failed:`, e));
            this.tabServices.delete(key);
          }
          attrs.onTabClose(key);
        },
        onTabReorder: (draggedKey, beforeKey) =>
          attrs.onTabReorder(draggedKey, beforeKey),
      }),
    );
  }
}
