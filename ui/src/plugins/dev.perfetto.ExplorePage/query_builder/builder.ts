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

// QUERY EXECUTION MODEL
// ====================
//
// The Explore Page uses a two-phase execution model with centralized control
// in QueryExecutionService.processNode().
//
// CENTRALIZED ARCHITECTURE
// ------------------------
// All autoExecute logic is handled by QueryExecutionService.processNode():
//
// | autoExecute | manual | Behavior                                      |
// |-------------|--------|-----------------------------------------------|
// | true        | false  | Analyze + execute if query changed            |
// | true        | true   | Analyze + execute (forced)                    |
// | false       | false  | Skip everything (save engine queries)         |
// | false       | true   | Analyze + execute (user clicked "Run Query")  |
//
// PHASE 1: ANALYSIS (Validation)
// ------------------------------
// When node state changes, NodeExplorer calls service.processNode({ manual: false }).
// The service decides whether to analyze based on autoExecute flag.
// If analysis runs:
// 1. Sends structured queries to the engine
// 2. Engine VALIDATES the query and returns generated SQL (doesn't execute)
// 3. Returns a Query object: {sql, textproto, modules, preambles, columns}
//
// PHASE 2: EXECUTION (Materialization)
// ------------------------------------
// The service decides whether to execute based on autoExecute and manual flags.
// If execution runs:
// 1. Materializes the query into a PERFETTO table
// 2. SQL = modules + preambles + query.sql
// 3. Table name: _exp_materialized_{sanitizedNodeId}
// 4. Creates SQLDataSource pointing to the materialized table
// 5. Fetches metadata (COUNT and column info) from materialized table
// 6. SQLDataSource handles server-side pagination, filtering, sorting
// 7. Updates node.state.issues with any errors/warnings
// 8. For SqlSourceNode, updates available columns
//
// Auto-execute is set to FALSE for:
// - SqlSourceNode: User writes SQL manually, should control execution
// - IntervalIntersectNode: Multi-node operation, potentially expensive
// - UnionNode: Multi-node operation, potentially expensive
// - FilterDuringNode: Multi-node operation, potentially expensive
//
// STATE MANAGEMENT
// ---------------
// - this.query: Current validated query (from analysis phase)
//   * Single source of truth for query state
//   * Updated by both automatic analysis (NodeExplorer) and manual execution (Builder)
//   * Passed to NodeExplorer as a prop for rendering SQL/Proto tabs
// - this.queryExecuted: Flag to prevent duplicate execution
// - this.response: Query results from execution
// - this.dataSource: Wrapped data source for DataGrid display
//
// QUERY STATE FLOW
// ----------------
// Automatic execution (autoExecute=true):
//   1. NodeExplorer.updateQuery() → processNode({ manual: false })
//   2. onAnalysisComplete → sets NodeExplorer.currentQuery
//   3. onAnalysisComplete → calls onQueryAnalyzed callback → sets Builder.query
//   4. Builder passes query as prop to NodeExplorer
//   5. NodeExplorer.renderContent() uses attrs.query ?? this.currentQuery
//
// Manual execution (autoExecute=false):
//   1. User clicks "Run Query" button → Builder calls processNode({ manual: true })
//   2. onAnalysisComplete → sets Builder.query
//   3. onAnalysisComplete → calls onNodeQueryAnalyzed callback → sets Builder.query
//   4. Builder passes query as prop to NodeExplorer
//   5. NodeExplorer.renderContent() uses attrs.query (this.currentQuery may be undefined)
//
// This ensures SQL/Proto tabs display correctly for both automatic and manual execution.
//
// RACE CONDITION PREVENTION
// -------------------------
// The onNodeQueryAnalyzed callback captures the selected node at creation time to prevent
// stale query leakage when rapidly switching between nodes:
//
// Without validation:
//   1. User selects Node A → async analysis starts → captures callback
//   2. User quickly switches to Node B → Node A destroyed, new callback created
//   3. Node A's analysis completes in background → old callback fires
//   4. Old callback sets this.query = queryForNodeA
//   5. Node B incorrectly displays Node A's query
//
// With validation (callbackNode === this.previousSelectedNode):
//   - Old callbacks from destroyed nodes are ignored
//   - Only queries from the currently selected node update the display

import m from 'mithril';
import {classNames} from '../../../base/classnames';
import {Button} from '../../../widgets/button';
import {Icons} from '../../../base/semantic_icons';
import {Icon} from '../../../widgets/icon';
import {Trace} from '../../../public/trace';
import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {QueryNode, Query} from '../query_node';
import {getPrimarySelectedNode} from '../selection_utils';
import {isAQuery, queryToRun} from './query_builder_utils';
import {NodeExplorer} from './node_explorer';
import {Graph} from './graph/graph';
import {DataExplorer} from './data_explorer';
import {
  DrawerPanel,
  DrawerPanelVisibility,
} from '../../../widgets/drawer_panel';
import {SQLDataSource} from '../../../components/widgets/datagrid/sql_data_source';
import {createSimpleSchema} from '../../../components/widgets/datagrid/sql_schema';
import {QueryResponse} from '../../../components/query_table/queries';
import {addQueryResultsTab} from '../../../components/query_table/query_result_tab';
import {SqlSourceNode} from './nodes/sources/sql_source';
import {findErrors, findWarnings} from './query_builder_utils';
import {NodeIssues} from './node_issues';
import {DataExplorerEmptyState, RoundActionButton} from './widgets';
import {UIFilter} from './operations/filter';
import {QueryExecutionService} from './query_execution_service';
import {Column} from '../../../components/widgets/datagrid/model';
import {ResizeHandle} from '../../../widgets/resize_handle';
import {getAllDownstreamNodes, getAllNodes} from './graph_utils';
import {Popup, PopupPosition} from '../../../widgets/popup';
import {DataSource} from '../../../components/widgets/datagrid/data_source';
import {NavigationSidePanel} from './navigation_sidepanel';

// Side panel width - must match --pf-qb-side-panel-width in builder.scss
const SIDE_PANEL_WIDTH = 60;

export interface BuilderAttrs {
  readonly trace: Trace;
  readonly sqlModules: SqlModules;
  readonly queryExecutionService: QueryExecutionService;

  readonly rootNodes: QueryNode[];
  readonly selectedNodes: ReadonlySet<string>;
  readonly nodeLayouts: Map<string, {x: number; y: number}>;
  readonly labels: ReadonlyArray<{
    id: string;
    x: number;
    y: number;
    width: number;
    text: string;
  }>;
  readonly loadGeneration?: number;
  readonly isExplorerCollapsed?: boolean;
  readonly sidebarWidth?: number;

  // Add nodes.
  readonly onAddSourceNode: (id: string) => void;
  readonly onAddOperationNode: (id: string, node: QueryNode) => void;

  readonly onRootNodeCreated: (node: QueryNode) => void;
  readonly onNodeSelected: (node?: QueryNode) => void;
  readonly onNodeAddToSelection: (node: QueryNode) => void;
  readonly onNodeRemoveFromSelection: (nodeId: string) => void;
  readonly onDeselect: () => void;
  readonly onNodeLayoutChange: (
    nodeId: string,
    layout: {x: number; y: number},
  ) => void;
  readonly onLabelsChange?: (
    labels: Array<{
      id: string;
      x: number;
      y: number;
      width: number;
      text: string;
    }>,
  ) => void;
  readonly onExplorerCollapsedChange?: (collapsed: boolean) => void;
  readonly onSidebarWidthChange?: (width: number) => void;

  readonly onDeleteNode: (node: QueryNode) => void;
  readonly onClearAllNodes: () => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onConnectionRemove: (
    fromNode: QueryNode,
    toNode: QueryNode,
    isSecondaryInput: boolean,
  ) => void;
  readonly onFilterAdd: (
    node: QueryNode,
    filter: UIFilter | UIFilter[],
    filterOperator?: 'AND' | 'OR',
  ) => void;
  readonly onColumnAdd?: (node: QueryNode, column: Column) => void;

  // Import / Export JSON
  readonly onImport: () => void;
  readonly onExport: () => void;

  // Starting templates (when page is empty)
  readonly onLoadEmptyTemplate?: () => void;
  readonly onLoadExampleByPath?: (jsonPath: string) => void;
  readonly onLoadExploreTemplate?: () => void;
  readonly onLoadRecentGraph?: (json: string) => void;

  // Node state change callback
  readonly onNodeStateChange?: () => void;

  // Undo / Redo
  readonly onUndo?: () => void;
  readonly onRedo?: () => void;
  readonly canUndo?: boolean;
  readonly canRedo?: boolean;

  // Called when the execute function is ready (or cleared).
  // Allows parent to trigger query execution via keyboard shortcuts (Ctrl+Enter).
  // Receives undefined when no node is selected.
  readonly onExecuteReady?: (
    executeFn: (() => Promise<void>) | undefined,
  ) => void;
}

enum SelectedView {
  kInfo = 0,
  kModify = 1,
  kResult = 2,
}

export class Builder implements m.ClassComponent<BuilderAttrs> {
  private trace: Trace;
  private queryExecutionService: QueryExecutionService;
  private query?: Query | Error;
  private queryExecuted: boolean = false;
  private isQueryRunning: boolean = false;
  private isAnalyzing: boolean = false;
  private previousSelectedNode?: QueryNode;
  // Stores selected node for keyboard shortcuts. This duplicates attrs.selectedNode
  // because we need access outside of view() for executeSelectedNode() public method.
  // Updated in view() to stay synchronized with attrs.
  private selectedNode?: QueryNode;
  // Stores all nodes for use in executeSelectedNode() which needs to pass
  // allNodes to prevent auto-drop of disconnected graphs.
  private rootNodes: QueryNode[] = [];
  private response?: QueryResponse;
  private dataSource?: DataSource;
  private drawerVisibility = DrawerPanelVisibility.COLLAPSED;
  private selectedView: SelectedView = SelectedView.kInfo;
  private readonly MIN_SIDEBAR_WIDTH = 250;
  private readonly MAX_SIDEBAR_WIDTH = 800;
  private readonly DEFAULT_SIDEBAR_WIDTH = 500;
  private hasEverSelectedNode = false;
  private onNodeQueryAnalyzed?: (query: Query | Error | undefined) => void;

  constructor({attrs}: m.Vnode<BuilderAttrs>) {
    this.trace = attrs.trace;
    // Use the shared QueryExecutionService from parent
    this.queryExecutionService = attrs.queryExecutionService;
  }

  private handleSidebarResize(attrs: BuilderAttrs, deltaPx: number) {
    const currentWidth = attrs.sidebarWidth ?? this.DEFAULT_SIDEBAR_WIDTH;
    // Subtract delta because the handle is on the left edge of the sidebar
    // Dragging left (negative delta) = narrower sidebar (positive change)
    // Dragging right (positive delta) = wider sidebar (negative change)
    const newWidth = Math.max(
      this.MIN_SIDEBAR_WIDTH,
      Math.min(this.MAX_SIDEBAR_WIDTH, currentWidth - deltaPx),
    );
    attrs.onSidebarWidthChange?.(newWidth);
  }

  view({attrs}: m.CVnode<BuilderAttrs>) {
    const {trace, rootNodes, onNodeSelected, onClearAllNodes} = attrs;
    const selectedNode = getPrimarySelectedNode(attrs.selectedNodes, rootNodes);

    // Store selectedNode and rootNodes for keyboard shortcuts (executeSelectedNode)
    this.selectedNode = selectedNode;
    this.rootNodes = rootNodes;

    // Notify parent when execute function changes (when selectedNode changes)
    if (selectedNode !== this.previousSelectedNode) {
      if (selectedNode !== undefined) {
        // Provide execute function bound to the current instance
        attrs.onExecuteReady?.(() => this.executeSelectedNode());
      } else {
        // Clear execute function when no node is selected
        attrs.onExecuteReady?.(undefined);
      }
    }

    if (
      selectedNode !== undefined &&
      selectedNode !== this.previousSelectedNode
    ) {
      this.resetQueryState();
      this.isQueryRunning = false;
      this.isAnalyzing = false;

      // Show drawer the first time any node is selected
      if (!this.hasEverSelectedNode) {
        this.drawerVisibility = DrawerPanelVisibility.VISIBLE;
        this.hasEverSelectedNode = true;
      }

      const hasModifyPanel = selectedNode.nodeSpecificModify() != null;
      // If current view is Info, switch to Modify (if available) when selecting a new node
      if (this.selectedView === SelectedView.kInfo && hasModifyPanel) {
        this.selectedView = SelectedView.kModify;
      }
      // If current view is Modify but modify panel is not available, switch to Info
      if (this.selectedView === SelectedView.kModify && !hasModifyPanel) {
        this.selectedView = SelectedView.kInfo;
      }
    }

    const isExplorerCollapsed = attrs.isExplorerCollapsed ?? false;
    const sidebarWidth = attrs.sidebarWidth ?? this.DEFAULT_SIDEBAR_WIDTH;

    // When transitioning to unselected state with collapsed explorer, reappear at minimum size
    if (
      selectedNode === undefined &&
      this.previousSelectedNode !== undefined &&
      isExplorerCollapsed
    ) {
      attrs.onExplorerCollapsedChange?.(false);
      attrs.onSidebarWidthChange?.(this.MIN_SIDEBAR_WIDTH);
    }

    this.previousSelectedNode = selectedNode;

    const layoutClasses =
      classNames(
        'pf-query-builder-layout',
        isExplorerCollapsed && 'explorer-collapsed',
      ) || '';

    // Create the onQueryAnalyzed callback and save it so manual execution can also use it
    // Capture the current selected node to prevent race conditions when switching nodes rapidly
    const callbackNode = selectedNode;
    this.onNodeQueryAnalyzed = (query: Query | Error | undefined) => {
      // Only update query if we're still on the same node
      // This prevents stale queries from Node A being applied when we've switched to Node B
      if (callbackNode === this.previousSelectedNode) {
        this.query = query;
      }
    };

    const hasMultipleNodesSelected = attrs.selectedNodes.size > 1;

    const explorer = hasMultipleNodesSelected
      ? m(
          '.pf-unselected-explorer',
          m(DataExplorerEmptyState, {
            icon: 'info',
            title: `${attrs.selectedNodes.size} nodes selected`,
          }),
        )
      : selectedNode
        ? m(NodeExplorer, {
            // The key to force mithril to re-create the component when the
            // selected node changes, preventing state from leaking between
            // different nodes.
            key: selectedNode.nodeId,
            trace,
            node: selectedNode,
            queryExecutionService: this.queryExecutionService,
            allNodes: getAllNodes(rootNodes),
            resolveNode: (nodeId: string) =>
              this.resolveNode(nodeId, rootNodes),
            hasExistingResult: this.queryExecuted,
            query: this.query, // Pass the query state from Builder (single source of truth)
            onQueryAnalyzed: this.onNodeQueryAnalyzed,
            onAnalysisStateChange: (isAnalyzing: boolean) => {
              this.isAnalyzing = isAnalyzing;
              if (isAnalyzing) {
                // Clear dataSource at the START of analysis to prevent stale
                // queries with old columns while the table is being re-materialized.
                this.dataSource = undefined;
              }
            },
            onExecutionStart: () => {
              this.isQueryRunning = true;
              this.queryExecuted = false;
            },
            onExecutionSuccess: (result) => {
              this.handleExecutionSuccess(selectedNode, result);
            },
            onExecutionError: (error) => {
              this.handleQueryError(selectedNode, error);
              this.isQueryRunning = false;
              m.redraw();
            },
            onchange: () => {
              // When a node's state changes, notify all downstream nodes
              // to update their columns and UI. This ensures that when e.g.
              // a column is renamed in ModifyColumnsNode, the AggregationNode
              // sees the new column name.
              const downstreamNodes = getAllDownstreamNodes(selectedNode);
              for (const node of downstreamNodes) {
                // Skip the node itself (it's included in downstream nodes)
                if (node.nodeId === selectedNode.nodeId) continue;
                node.onPrevNodesUpdated?.();
              }
              attrs.onNodeStateChange?.();
            },
            isCollapsed: isExplorerCollapsed,
            selectedView: this.selectedView,
            onViewChange: (view: number) => {
              this.selectedView = view;
            },
          })
        : m(
            '.pf-unselected-explorer',
            m(NavigationSidePanel, {
              selectedNode,
              onAddSourceNode: attrs.onAddSourceNode,
              onLoadExampleByPath: attrs.onLoadExampleByPath,
              onLoadExploreTemplate: attrs.onLoadExploreTemplate,
              onLoadEmptyTemplate: attrs.onLoadEmptyTemplate,
              onLoadRecentGraph: attrs.onLoadRecentGraph,
            }),
          );

    return m(DrawerPanel, {
      className: layoutClasses,
      visibility: this.drawerVisibility,
      onVisibilityChange: (v) => {
        this.drawerVisibility = v;
      },
      startingHeight: 300,
      drawerContent: hasMultipleNodesSelected
        ? m(DataExplorerEmptyState, {
            icon: 'info',
            title: `${attrs.selectedNodes.size} nodes selected`,
          })
        : selectedNode
          ? m(DataExplorer, {
              trace: this.trace,
              query: this.query,
              node: selectedNode,
              response: this.response,
              dataSource: this.dataSource,
              sqlModules: attrs.sqlModules,
              queryExecutionService: this.queryExecutionService,
              isQueryRunning: this.isQueryRunning,
              isAnalyzing: this.isAnalyzing,
              isStale: this.queryExecutionService.isNodeStale(
                selectedNode.nodeId,
              ),
              onchange: () => {
                attrs.onNodeStateChange?.();
              },
              onFilterAdd: (filter, filterOperator) => {
                attrs.onFilterAdd(selectedNode, filter, filterOperator);
              },
              onColumnAdd: attrs.onColumnAdd
                ? (column) => attrs.onColumnAdd?.(selectedNode, column)
                : undefined,
              isFullScreen:
                this.drawerVisibility === DrawerPanelVisibility.FULLSCREEN,
              onFullScreenToggle: () => {
                if (
                  this.drawerVisibility === DrawerPanelVisibility.FULLSCREEN
                ) {
                  this.drawerVisibility = DrawerPanelVisibility.VISIBLE;
                } else {
                  this.drawerVisibility = DrawerPanelVisibility.FULLSCREEN;
                }
              },
              onExecute: async () => {
                await this.executeSelectedNode();
              },
              onExportToTimeline: async () => {
                await this.exportToTimeline(selectedNode);
              },
            })
          : m(DataExplorerEmptyState, {
              icon: 'info',
              title: 'Select a node to see the data',
            }),
      mainContent: [
        m(
          '.pf-qb-node-graph',
          m(Graph, {
            rootNodes,
            selectedNodes: attrs.selectedNodes,
            onNodeSelected,
            onNodeAddToSelection: attrs.onNodeAddToSelection,
            onNodeRemoveFromSelection: attrs.onNodeRemoveFromSelection,
            nodeLayouts: attrs.nodeLayouts,
            labels: attrs.labels,
            loadGeneration: attrs.loadGeneration,
            onNodeLayoutChange: attrs.onNodeLayoutChange,
            onLabelsChange: attrs.onLabelsChange,
            onDeselect: attrs.onDeselect,
            onAddSourceNode: attrs.onAddSourceNode,
            onClearAllNodes,
            onDuplicateNode: attrs.onDuplicateNode,
            onAddOperationNode: (id, node) =>
              attrs.onAddOperationNode(id, node),
            onDeleteNode: attrs.onDeleteNode,
            onConnectionRemove: attrs.onConnectionRemove,
            onImport: attrs.onImport,
            onExport: attrs.onExport,
          }),
          selectedNode &&
            m(
              '.pf-qb-floating-controls',
              !selectedNode.validate() &&
                m(
                  Popup,
                  {
                    trigger: m(
                      '.pf-qb-floating-warning',
                      m(Icon, {
                        icon: Icons.Warning,
                        filled: true,
                        className: 'pf-qb-warning-icon',
                        title: 'Click to see error details',
                      }),
                    ),
                    position: PopupPosition.BottomEnd,
                    showArrow: true,
                  },
                  m(
                    '.pf-error-details',
                    selectedNode.state.issues?.getTitle() ?? 'No error details',
                  ),
                ),
            ),
          m(
            '.pf-qb-floating-controls-bottom',
            attrs.onUndo &&
              RoundActionButton({
                icon: Icons.Undo,
                title: 'Undo (Ctrl+Z)',
                onclick: attrs.onUndo,
                disabled: !attrs.canUndo,
              }),
            attrs.onRedo &&
              RoundActionButton({
                icon: Icons.Redo,
                title: 'Redo (Ctrl+Shift+Z)',
                onclick: attrs.onRedo,
                disabled: !attrs.canRedo,
              }),
          ),
        ),
        m(ResizeHandle, {
          direction: 'horizontal',
          onResize: (deltaPx) => this.handleSidebarResize(attrs, deltaPx),
        }),
        m(
          '.pf-qb-explorer',
          {
            style: {
              width: isExplorerCollapsed
                ? '0'
                : `${sidebarWidth + (selectedNode ? 0 : SIDE_PANEL_WIDTH)}px`,
            },
          },
          explorer,
        ),
        selectedNode &&
          m(
            '.pf-qb-side-panel',
            m(Button, {
              icon: Icons.Info,
              title: 'Info',
              className:
                this.selectedView === SelectedView.kInfo && !isExplorerCollapsed
                  ? 'pf-active'
                  : '',
              onclick: () => {
                if (
                  this.selectedView === SelectedView.kInfo &&
                  !isExplorerCollapsed
                ) {
                  attrs.onExplorerCollapsedChange?.(true);
                } else {
                  this.selectedView = SelectedView.kInfo;
                  attrs.onExplorerCollapsedChange?.(false);
                }
              },
            }),
            selectedNode.nodeSpecificModify() != null &&
              m(Button, {
                icon: Icons.Edit,
                title: 'Edit',
                className:
                  this.selectedView === SelectedView.kModify &&
                  !isExplorerCollapsed
                    ? 'pf-active'
                    : '',
                onclick: () => {
                  if (
                    this.selectedView === SelectedView.kModify &&
                    !isExplorerCollapsed
                  ) {
                    attrs.onExplorerCollapsedChange?.(true);
                  } else {
                    this.selectedView = SelectedView.kModify;
                    attrs.onExplorerCollapsedChange?.(false);
                  }
                },
              }),
            m(Button, {
              icon: 'code',
              title: 'Result',
              className:
                this.selectedView === SelectedView.kResult &&
                !isExplorerCollapsed
                  ? 'pf-active'
                  : '',
              onclick: () => {
                if (
                  this.selectedView === SelectedView.kResult &&
                  !isExplorerCollapsed
                ) {
                  attrs.onExplorerCollapsedChange?.(true);
                } else {
                  this.selectedView = SelectedView.kResult;
                  attrs.onExplorerCollapsedChange?.(false);
                }
              },
            }),
          ),
      ],
    });
  }

  private resolveNode(
    nodeId: string,
    rootNodes: QueryNode[],
  ): QueryNode | undefined {
    const queue: QueryNode[] = [...rootNodes];
    const visited = new Set<string>();

    while (queue.length > 0) {
      const current = queue.shift()!;
      if (visited.has(current.nodeId)) {
        continue;
      }
      visited.add(current.nodeId);

      if (current.nodeId === nodeId) {
        return current;
      }

      queue.push(...current.nextNodes);
    }
    return undefined;
  }

  /**
   * Handles successful query execution by updating UI state.
   * Called from both automatic execution (via NodeExplorer) and manual execution (via onExecute).
   */
  private handleExecutionSuccess(
    node: QueryNode,
    result: {
      tableName: string;
      rowCount: number;
      columns: string[];
      durationMs: number;
    },
  ) {
    const engine = this.queryExecutionService.getEngine();
    const query = this.query;

    this.response = {
      query: isAQuery(query) ? queryToRun(query) : '',
      totalRowCount: result.rowCount,
      durationMs: result.durationMs,
      columns: result.columns,
      rows: [],
      statementCount: 1,
      statementWithOutputCount: 1,
      lastStatementSql: isAQuery(query) ? query.sql : '',
    };

    this.dataSource = new SQLDataSource({
      engine,
      sqlSchema: createSimpleSchema(result.tableName),
      rootSchemaName: 'query',
    });
    this.queryExecuted = true;
    this.isQueryRunning = false;

    if (isAQuery(query)) {
      this.setNodeIssuesFromResponse(node, query, this.response);
    }

    if (node instanceof SqlSourceNode && this.response !== undefined) {
      node.onQueryExecuted(this.response.columns);
    }

    m.redraw();
  }

  /**
   * Creates callbacks for processNode() when manually executing a query.
   * Used by onExecute to avoid duplicating callback logic.
   */
  private createManualExecutionCallbacks(node: QueryNode) {
    return {
      onAnalysisStart: () => {
        this.isAnalyzing = true;
        // Clear dataSource at the START of analysis to prevent stale
        // queries with old columns while the table is being re-materialized.
        this.dataSource = undefined;
        m.redraw();
      },
      onAnalysisComplete: (query: Query | Error | undefined) => {
        // Update query state via callback (with validation to prevent stale query leakage)
        this.onNodeQueryAnalyzed?.(query);
        this.isAnalyzing = false;
        m.redraw();
      },
      onExecutionStart: () => {
        this.isQueryRunning = true;
        this.queryExecuted = false;
        m.redraw();
      },
      onExecutionSuccess: (result: {
        tableName: string;
        rowCount: number;
        columns: string[];
        durationMs: number;
      }) => {
        this.handleExecutionSuccess(node, result);
      },
      onExecutionError: (error: unknown) => {
        this.handleQueryError(node, error);
        this.isQueryRunning = false;
        m.redraw();
      },
    };
  }

  /**
   * Executes the selected node's query manually.
   * Public method called when user clicks "Run Query" button or presses Ctrl+Enter
   * keyboard shortcut (invoked via parent component's keyboard handler).
   */
  async executeSelectedNode(): Promise<void> {
    const selectedNode = this.selectedNode;
    if (selectedNode === undefined) {
      return;
    }

    if (!selectedNode.validate()) {
      console.warn(
        `Cannot execute query: node ${selectedNode.nodeId} failed validation`,
      );
      return;
    }

    // Use the centralized service with manual=true.
    // The service handles both analysis and execution.
    await this.queryExecutionService.processNode(
      selectedNode,
      this.trace.engine,
      getAllNodes(this.rootNodes),
      {
        manual: true, // User explicitly requested execution
        hasExistingResult: this.queryExecuted,
        ...this.createManualExecutionCallbacks(selectedNode),
      },
    );
  }

  private async exportToTimeline(node: QueryNode) {
    // Fetch table name from TP
    const tableName = await this.queryExecutionService.getTableName(
      node.nodeId,
    );
    if (!tableName) {
      console.warn('Cannot export to timeline: no materialized table');
      return;
    }

    // Use the materialized table instead of re-running the original query
    addQueryResultsTab(
      this.trace,
      {
        query: `SELECT * FROM ${tableName}`,
        title: 'Explore Query',
      },
      'explore_page',
    );

    // Navigate to the timeline page
    this.trace.navigate('#!/viewer');
  }

  private setNodeIssuesFromResponse(
    node: QueryNode,
    query: Query,
    response: QueryResponse,
  ) {
    const error = findErrors(query, response);
    const warning = findWarnings(response, node);
    const noDataWarning =
      response.totalRowCount === 0
        ? new Error('Query returned no rows')
        : undefined;

    if (error || warning || noDataWarning) {
      if (!node.state.issues) {
        node.state.issues = new NodeIssues();
      }
      node.state.issues.queryError = error;
      node.state.issues.responseError = warning;
      node.state.issues.dataError = noDataWarning;
      // Clear any previous execution error since we got a successful response
      node.state.issues.clearExecutionError();
    } else {
      node.state.issues = undefined;
    }
  }

  /**
   * Resets the query execution state.
   * Used when switching nodes or after query execution errors.
   */
  private resetQueryState() {
    this.dataSource = undefined;
    this.response = undefined;
    this.query = undefined;
    this.queryExecuted = false;
    // Clear any pending execution in the service
    this.queryExecutionService.clearPendingExecution();
  }

  private handleQueryError(node: QueryNode, e: unknown) {
    console.error('Failed to run query:', e);
    // Clear response and data source but keep query so Retry can re-execute
    this.dataSource = undefined;
    this.response = undefined;
    this.queryExecuted = false;
    if (!node.state.issues) {
      node.state.issues = new NodeIssues();
    }
    // Use executionError (not queryError) so error persists across re-renders
    // that trigger validate() - queryError gets cleared during validation
    node.state.issues.executionError =
      e instanceof Error ? e : new Error(String(e));
  }
}
