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
// The Explore Page uses a two-phase execution model:
//
// PHASE 1: ANALYSIS (Validation)
// ------------------------------
// When a node's state changes:
// 1. NodeExplorer.updateQuery() is called (debounced via AsyncLimiter)
// 2. Calls analyzeNode() which sends structured queries to the engine
// 3. Engine VALIDATES the query and returns generated SQL (doesn't execute)
// 4. Returns a Query object: {sql, textproto, modules, preambles, columns}
// 5. Calls onQueryAnalyzed() callback with the validated query
//
// PHASE 2: EXECUTION (Running) & MATERIALIZATION
// -----------------------------------------------
// After analysis, execution happens based on node.state.autoExecute:
// - If autoExecute = true (default): Query runs automatically
// - If autoExecute = false: User must click "Run" button
//
// Auto-execute is set to FALSE for:
// - SqlSourceNode: User writes SQL manually, should control execution
// - IntervalIntersectNode: Multi-node operation, potentially expensive
// - UnionNode: Multi-node operation, potentially expensive
//
// Execution flow:
// 1. Builder.runQuery() is called (auto or manual)
// 2. Materializes the query into a PERFETTO table
// 3. SQL = modules + preambles + query.sql
// 4. Table name: _exp_materialized_{sanitizedNodeId}
// 5. Creates SQLDataSource pointing to the materialized table
// 6. Fetches metadata (COUNT and column info) from materialized table
// 7. SQLDataSource handles server-side pagination, filtering, sorting
// 8. Updates node.state.issues with any errors/warnings
// 9. For SqlSourceNode, updates available columns
//
// STATE MANAGEMENT
// ---------------
// - this.query: Current validated query (from analysis phase)
// - this.queryExecuted: Flag to prevent duplicate execution
// - this.response: Query results from execution
// - this.dataSource: Wrapped data source for DataGrid display

import m from 'mithril';
import {classNames} from '../../../base/classnames';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Icons} from '../../../base/semantic_icons';
import {Intent} from '../../../widgets/common';
import {Icon} from '../../../widgets/icon';
import {Card} from '../../../widgets/card';
import {Keycap} from '../../../widgets/hotkey_glyphs';
import {Trace} from '../../../public/trace';
import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {QueryNode, Query, isAQuery, queryToRun} from '../query_node';
import {NodeExplorer} from './node_explorer';
import {Graph} from './graph/graph';
import {DataExplorer} from './data_explorer';
import {
  SplitPanel,
  SplitPanelDrawerVisibility,
} from '../../../widgets/split_panel';
import {DataGridDataSource} from '../../../components/widgets/data_grid/common';
import {SQLDataSource} from '../../../components/widgets/data_grid/sql_data_source';
import {QueryResponse} from '../../../components/query_table/queries';
import {addQueryResultsTab} from '../../../components/query_table/query_result_tab';
import {SqlSourceNode} from './nodes/sources/sql_source';
import {findErrors, findWarnings} from './query_builder_utils';
import {NodeIssues} from './node_issues';
import {DataExplorerEmptyState} from './widgets';
import {UIFilter} from './operations/filter';
import {QueryExecutionService} from './query_execution_service';
import {ResizeHandle} from '../../../widgets/resize_handle';
import {nodeRegistry} from './node_registry';
import {getAllDownstreamNodes} from './graph_utils';
import {Popup, PopupPosition} from '../../../widgets/popup';

// Side panel width - must match --pf-qb-side-panel-width in builder.scss
const SIDE_PANEL_WIDTH = 60;

export interface BuilderAttrs {
  readonly trace: Trace;
  readonly sqlModules: SqlModules;
  readonly queryExecutionService: QueryExecutionService;

  readonly rootNodes: QueryNode[];
  readonly selectedNode?: QueryNode;
  readonly nodeLayouts: Map<string, {x: number; y: number}>;

  // Add nodes.
  readonly onAddSourceNode: (id: string) => void;
  readonly onAddOperationNode: (id: string, node: QueryNode) => void;

  readonly onRootNodeCreated: (node: QueryNode) => void;
  readonly onNodeSelected: (node?: QueryNode) => void;
  readonly onDeselect: () => void;
  readonly onNodeLayoutChange: (
    nodeId: string,
    layout: {x: number; y: number},
  ) => void;

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

  // Import / Export JSON
  readonly onImport: () => void;
  readonly onExport: () => void;

  readonly onLoadExample: () => void;

  // Node state change callback
  readonly onNodeStateChange?: () => void;

  // Undo / Redo
  readonly onUndo?: () => void;
  readonly onRedo?: () => void;
  readonly canUndo?: boolean;
  readonly canRedo?: boolean;
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
  private isExplorerCollapsed: boolean = false;
  private response?: QueryResponse;
  private dataSource?: DataGridDataSource;
  private drawerVisibility = SplitPanelDrawerVisibility.COLLAPSED;
  private selectedView: SelectedView = SelectedView.kInfo;
  private sidebarWidth: number = 500; // Default width in pixels
  private readonly MIN_SIDEBAR_WIDTH = 250;
  private readonly MAX_SIDEBAR_WIDTH = 800;

  constructor({attrs}: m.Vnode<BuilderAttrs>) {
    this.trace = attrs.trace;
    // Use the shared QueryExecutionService from parent
    this.queryExecutionService = attrs.queryExecutionService;
  }

  private handleSidebarResize(deltaPx: number) {
    // Subtract delta because the handle is on the left edge of the sidebar
    // Dragging left (negative delta) = narrower sidebar (positive change)
    // Dragging right (positive delta) = wider sidebar (negative change)
    this.sidebarWidth = Math.max(
      this.MIN_SIDEBAR_WIDTH,
      Math.min(this.MAX_SIDEBAR_WIDTH, this.sidebarWidth - deltaPx),
    );
    m.redraw();
  }


  private renderSourceCards(attrs: BuilderAttrs): m.Children {
    const sourceNodes = nodeRegistry
      .list()
      .filter(([_id, node]) => node.showOnLandingPage === true)
      .map(([id, node]) => {
        const name = node.name ?? 'Unnamed Source';
        const description = node.description ?? '';
        const icon = node.icon ?? '';
        const hotkey =
          node.hotkey && typeof node.hotkey === 'string'
            ? node.hotkey.toUpperCase()
            : undefined;

        return m(
          Card,
          {
            'interactive': true,
            'onclick': () => attrs.onAddSourceNode(id),
            'tabindex': 0,
            'role': 'button',
            'aria-label': `Add ${name} source`,
            'className': 'pf-source-card',
            'onkeydown': (e: KeyboardEvent) => {
              if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault();
                attrs.onAddSourceNode(id);
              }
            },
          },
          m('.pf-source-card-clickable', m(Icon, {icon}), m('h3', name)),
          m('p', description),
          hotkey ? m('.pf-source-card-hotkey', m(Keycap, hotkey)) : null,
        );
      });

    // Add Examples card at the end
    const examplesCard = m(
      Card,
      {
        'interactive': true,
        'onclick': () => attrs.onLoadExample(),
        'tabindex': 0,
        'role': 'button',
        'aria-label': 'Load example graph',
        'className': 'pf-source-card',
        'onkeydown': (e: KeyboardEvent) => {
          if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            attrs.onLoadExample();
          }
        },
      },
      m(
        '.pf-source-card-clickable',
        m(Icon, {icon: 'auto_stories'}),
        m('h3', 'Examples'),
      ),
      m('p', 'Load an example graph'),
    );

    if (sourceNodes.length === 0) {
      return [examplesCard];
    }

    return [examplesCard, ...sourceNodes];
  }

  view({attrs}: m.CVnode<BuilderAttrs>) {
    const {trace, rootNodes, onNodeSelected, selectedNode, onClearAllNodes} =
      attrs;

    if (selectedNode && selectedNode !== this.previousSelectedNode) {
      this.resetQueryState();
      this.isQueryRunning = false;
      this.isAnalyzing = false;
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

    // When transitioning to unselected state with collapsed explorer, reappear at minimum size
    if (
      !selectedNode &&
      this.previousSelectedNode &&
      this.isExplorerCollapsed
    ) {
      this.isExplorerCollapsed = false;
      this.sidebarWidth = this.MIN_SIDEBAR_WIDTH;
    }

    this.previousSelectedNode = selectedNode;

    const layoutClasses =
      classNames(
        'pf-query-builder-layout',
        this.isExplorerCollapsed && 'explorer-collapsed',
      ) || '';

    const explorer = selectedNode
      ? m(NodeExplorer, {
          // The key to force mithril to re-create the component when the
          // selected node changes, preventing state from leaking between
          // different nodes.
          key: selectedNode.nodeId,
          trace,
          node: selectedNode,
          resolveNode: (nodeId: string) => this.resolveNode(nodeId, rootNodes),
          onQueryAnalyzed: (query: Query | Error) => {
            this.query = query;
            if (isAQuery(this.query) && selectedNode.validate()) {
              this.runQuery(selectedNode, this.query, {manualExecution: false});
            }
          },
          onAnalysisStateChange: (isAnalyzing: boolean) => {
            this.isAnalyzing = isAnalyzing;
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
          isCollapsed: this.isExplorerCollapsed,
          selectedView: this.selectedView,
          onViewChange: (view: number) => {
            this.selectedView = view;
          },
        })
      : m('.pf-unselected-explorer', this.renderSourceCards(attrs));

    return m(
      SplitPanel,
      {
        className: layoutClasses,
        visibility: this.drawerVisibility,
        onVisibilityChange: (v) => {
          this.drawerVisibility = v;
        },
        startingHeight: 300,
        drawerContent: selectedNode
          ? m(DataExplorer, {
              trace: this.trace,
              query: this.query,
              node: selectedNode,
              response: this.response,
              dataSource: this.dataSource,
              isQueryRunning: this.isQueryRunning,
              isAnalyzing: this.isAnalyzing,
              onchange: () => {
                attrs.onNodeStateChange?.();
              },
              onFilterAdd: (filter, filterOperator) => {
                attrs.onFilterAdd(selectedNode, filter, filterOperator);
              },
              isFullScreen:
                this.drawerVisibility === SplitPanelDrawerVisibility.FULLSCREEN,
              onFullScreenToggle: () => {
                if (
                  this.drawerVisibility ===
                  SplitPanelDrawerVisibility.FULLSCREEN
                ) {
                  this.drawerVisibility = SplitPanelDrawerVisibility.VISIBLE;
                } else {
                  this.drawerVisibility = SplitPanelDrawerVisibility.FULLSCREEN;
                }
              },
              onExecute: () => {
                if (
                  !selectedNode.validate() ||
                  this.query === undefined ||
                  this.query instanceof Error ||
                  !isAQuery(this.query)
                ) {
                  return;
                }
                this.queryExecuted = false;
                this.runQuery(selectedNode, this.query, {
                  manualExecution: true,
                });
              },
              onExportToTimeline: () => {
                this.exportToTimeline(selectedNode);
              },
            })
          : m(DataExplorerEmptyState, {
              icon: 'info',
              title: 'Select a node to see the data',
            }),
      },
      m(
        '.pf-qb-node-graph',
        m(Graph, {
          rootNodes,
          selectedNode,
          onNodeSelected,
          nodeLayouts: attrs.nodeLayouts,
          onNodeLayoutChange: attrs.onNodeLayoutChange,
          onDeselect: attrs.onDeselect,
          onAddSourceNode: attrs.onAddSourceNode,
          onClearAllNodes,
          onDuplicateNode: attrs.onDuplicateNode,
          onAddOperationNode: (id, node) => attrs.onAddOperationNode(id, node),
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
            m(Button, {
              icon: Icons.Undo,
              title: 'Undo (Ctrl+Z)',
              onclick: attrs.onUndo,
              disabled: !attrs.canUndo,
              variant: ButtonVariant.Filled,
              rounded: true,
              iconFilled: true,
              intent: Intent.Primary,
            }),
          attrs.onRedo &&
            m(Button, {
              icon: Icons.Redo,
              title: 'Redo (Ctrl+Shift+Z)',
              onclick: attrs.onRedo,
              disabled: !attrs.canRedo,
              variant: ButtonVariant.Filled,
              rounded: true,
              iconFilled: true,
              intent: Intent.Primary,
            }),
        ),
      ),
      m(ResizeHandle, {
        direction: 'horizontal',
        onResize: (deltaPx) => this.handleSidebarResize(deltaPx),
      }),
      m(
        '.pf-qb-explorer',
        {
          style: {
            width: this.isExplorerCollapsed
              ? '0'
              : `${this.sidebarWidth + (selectedNode ? 0 : SIDE_PANEL_WIDTH)}px`,
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
              this.selectedView === SelectedView.kInfo &&
              !this.isExplorerCollapsed
                ? 'pf-active'
                : '',
            onclick: () => {
              if (
                this.selectedView === SelectedView.kInfo &&
                !this.isExplorerCollapsed
              ) {
                this.isExplorerCollapsed = true;
              } else {
                this.selectedView = SelectedView.kInfo;
                this.isExplorerCollapsed = false;
              }
            },
          }),
          selectedNode.nodeSpecificModify() != null &&
            m(Button, {
              icon: Icons.Edit,
              title: 'Edit',
              className:
                this.selectedView === SelectedView.kModify &&
                !this.isExplorerCollapsed
                  ? 'pf-active'
                  : '',
              onclick: () => {
                if (
                  this.selectedView === SelectedView.kModify &&
                  !this.isExplorerCollapsed
                ) {
                  this.isExplorerCollapsed = true;
                } else {
                  this.selectedView = SelectedView.kModify;
                  this.isExplorerCollapsed = false;
                }
              },
            }),
          m(Button, {
            icon: 'code',
            title: 'Result',
            className:
              this.selectedView === SelectedView.kResult &&
              !this.isExplorerCollapsed
                ? 'pf-active'
                : '',
            onclick: () => {
              if (
                this.selectedView === SelectedView.kResult &&
                !this.isExplorerCollapsed
              ) {
                this.isExplorerCollapsed = true;
              } else {
                this.selectedView = SelectedView.kResult;
                this.isExplorerCollapsed = false;
              }
            },
          }),
        ),
    );
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

  private async runQuery(
    node: QueryNode,
    query: Query,
    options: {manualExecution: boolean},
  ) {
    await this.queryExecutionService.executeNodeQuery(node, query, {
      shouldAutoExecute: options.manualExecution
        ? true
        : node.state.autoExecute ?? true,
      hasExistingResult: this.queryExecuted,
      onStart: () => {
        this.isQueryRunning = true;
        this.queryExecuted = false;
      },
      onSuccess: (result) => {
        const engine = this.queryExecutionService.getEngine();

        this.response = {
          query: queryToRun(query),
          totalRowCount: result.rowCount,
          durationMs: result.durationMs,
          columns: result.columns,
          rows: [],
          statementCount: 1,
          statementWithOutputCount: 1,
          lastStatementSql: query.sql,
        };

        this.dataSource = new SQLDataSource(
          engine,
          `SELECT * FROM ${result.tableName}`,
        );
        this.queryExecuted = true;
        this.isQueryRunning = false;
        this.setNodeIssuesFromResponse(node, query, this.response);

        if (node instanceof SqlSourceNode && this.response !== undefined) {
          node.onQueryExecuted(this.response.columns);
        }

        m.redraw();
      },
      onError: (error) => {
        this.handleQueryError(node, error);
        this.isQueryRunning = false;
        m.redraw();
      },
    });
  }

  private exportToTimeline(node: QueryNode) {
    // Only export if we have a materialized table
    const tableName = node.state.materializationTableName;
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
