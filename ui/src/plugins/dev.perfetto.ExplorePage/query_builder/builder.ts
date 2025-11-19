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
// PHASE 2: EXECUTION (Running)
// ----------------------------
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
// 2. Calls queryService.runQuery() with the full SQL string
// 3. SQL = modules + preambles + query.sql
// 4. Creates InMemoryDataSource with results
// 5. Updates node.state.issues with any errors/warnings
// 6. For SqlSourceNode, updates available columns
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

import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {QueryNode, Query, isAQuery, queryToRun} from '../query_node';
import {ExplorePageHelp} from './help';
import {NodeExplorer} from './node_explorer';
import {Graph} from './graph/graph';
import {Trace} from 'src/public/trace';
import {DataExplorer} from './data_explorer';
import {
  SplitPanel,
  SplitPanelDrawerVisibility,
} from '../../../widgets/split_panel';
import {
  DataGridDataSource,
  DataGridModel,
} from '../../../components/widgets/data_grid/common';
import {InMemoryDataSource} from '../../../components/widgets/data_grid/in_memory_data_source';
import {QueryResponse} from '../../../components/query_table/queries';
import {TableSourceNode} from './nodes/sources/table_source';
import {SqlSourceNode} from './nodes/sources/sql_source';
import {QueryService} from './query_service';
import {findErrors, findWarnings} from './query_builder_utils';
import {NodeIssues} from './node_issues';
import {UIFilter} from './operations/filter';
import {MaterializationService} from './materialization_service';

export interface BuilderAttrs {
  readonly trace: Trace;
  readonly sqlModules: SqlModules;

  readonly devMode?: boolean;

  readonly rootNodes: QueryNode[];
  readonly selectedNode?: QueryNode;
  readonly nodeLayouts: Map<string, {x: number; y: number}>;

  readonly onDevModeChange?: (enabled: boolean) => void;

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
  readonly onConnectionRemove: (fromNode: QueryNode, toNode: QueryNode) => void;
  readonly onFilterAdd: (node: QueryNode, filter: UIFilter) => void;

  // Import / Export JSON
  readonly onImport: () => void;
  readonly onExport: () => void;

  readonly onImportWithStatement: () => void;

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
  kComment = 3,
}

export class Builder implements m.ClassComponent<BuilderAttrs> {
  private queryService: QueryService;
  private materializationService: MaterializationService;
  private query?: Query | Error;
  private queryExecuted: boolean = false;
  private isQueryRunning: boolean = false;
  private isAnalyzing: boolean = false;
  private previousSelectedNode?: QueryNode;
  private isExplorerCollapsed: boolean = false;
  private response?: QueryResponse;
  private dataSource?: DataGridDataSource;
  private drawerVisibility = SplitPanelDrawerVisibility.VISIBLE;
  private selectedView: SelectedView = SelectedView.kInfo;

  constructor({attrs}: m.Vnode<BuilderAttrs>) {
    this.queryService = new QueryService(attrs.trace.engine);
    this.materializationService = new MaterializationService(
      attrs.trace.engine,
    );
  }

  view({attrs}: m.CVnode<BuilderAttrs>) {
    const {
      trace,
      rootNodes,
      onNodeSelected,
      selectedNode,
      onClearAllNodes,
      sqlModules,
    } = attrs;

    if (selectedNode && selectedNode !== this.previousSelectedNode) {
      this.response = undefined;
      this.dataSource = undefined;
      this.query = undefined;
      this.queryExecuted = false;
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
    this.previousSelectedNode = selectedNode;

    const layoutClasses =
      classNames(
        'pf-query-builder-layout',
        this.isExplorerCollapsed && 'explorer-collapsed',
      ) || '';

    // When no nodes exist, show only the graph (which renders EmptyGraph)
    // without any panels or split layout
    if (rootNodes.length === 0) {
      return m(Graph, {
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
        devMode: attrs.devMode,
        onDevModeChange: attrs.onDevModeChange,
        onDeleteNode: attrs.onDeleteNode,
        onConnectionRemove: attrs.onConnectionRemove,
        onImport: attrs.onImport,
        onImportWithStatement: attrs.onImportWithStatement,
        onExport: attrs.onExport,
      });
    }

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
            const shouldAutoExecute = selectedNode.state.autoExecute ?? true;
            if (isAQuery(this.query) && shouldAutoExecute) {
              this.queryExecuted = false;
              this.runQuery(selectedNode);
            }
          },
          onAnalysisStateChange: (isAnalyzing: boolean) => {
            this.isAnalyzing = isAnalyzing;
          },
          onchange: () => {
            attrs.onNodeStateChange?.();
          },
          isCollapsed: this.isExplorerCollapsed,
          selectedView: this.selectedView,
          onViewChange: (view: number) => {
            this.selectedView = view;
          },
        })
      : m(ExplorePageHelp, {
          sqlModules,
          onTableClick: (tableName: string) => {
            const {onRootNodeCreated} = attrs;
            const sqlTable = sqlModules.getTable(tableName);
            if (!sqlTable) return;

            const newNode = new TableSourceNode({
              trace,
              sqlModules,
              sqlTable,
              filters: [],
            });
            newNode.state.autoExecute = true;
            onRootNodeCreated(newNode);
          },
        });

    return m(
      SplitPanel,
      {
        className: layoutClasses,
        visibility: selectedNode
          ? this.drawerVisibility
          : SplitPanelDrawerVisibility.COLLAPSED,
        onVisibilityChange: (v) => {
          this.drawerVisibility = v;
        },
        startingHeight: 300,
        drawerContent: selectedNode
          ? m(DataExplorer, {
              queryService: this.queryService,
              query: this.query,
              node: selectedNode,
              response: this.response,
              dataSource: this.dataSource,
              isQueryRunning: this.isQueryRunning,
              isAnalyzing: this.isAnalyzing,
              onchange: () => {
                attrs.onNodeStateChange?.();
              },
              onFilterAdd: (filter) => {
                attrs.onFilterAdd(selectedNode, filter);
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
                // Reset queryExecuted flag to allow re-execution after errors or config changes
                this.queryExecuted = false;
                this.runQuery(selectedNode);
              },
            })
          : null,
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
          devMode: attrs.devMode,
          onDevModeChange: attrs.onDevModeChange,
          onDeleteNode: attrs.onDeleteNode,
          onConnectionRemove: attrs.onConnectionRemove,
          onImport: attrs.onImport,
          onImportWithStatement: attrs.onImportWithStatement,
          onExport: attrs.onExport,
        }),
        selectedNode &&
          m(
            '.pf-qb-floating-controls',
            !selectedNode.validate() &&
              m(
                '.pf-qb-floating-warning',
                m(Icon, {
                  icon: Icons.Warning,
                  filled: true,
                  className: 'pf-qb-warning-icon',
                  title: `Invalid node: ${selectedNode.state.issues?.getTitle() ?? ''}`,
                }),
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
      m('.pf-qb-explorer', explorer),
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
          m(Button, {
            icon: 'comment',
            title: 'Comment',
            iconFilled: !!selectedNode.state.comment,
            className:
              this.selectedView === SelectedView.kComment &&
              !this.isExplorerCollapsed
                ? 'pf-active'
                : '',
            onclick: () => {
              if (
                this.selectedView === SelectedView.kComment &&
                !this.isExplorerCollapsed
              ) {
                this.isExplorerCollapsed = true;
              } else {
                this.selectedView = SelectedView.kComment;
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

  private async runQuery(node: QueryNode) {
    if (
      this.query === undefined ||
      this.query instanceof Error ||
      this.queryExecuted
    ) {
      return;
    }

    this.isQueryRunning = true;

    try {
      const response = await this.queryService.runQuery(queryToRun(this.query));
      this.response = response;
      const ds = new InMemoryDataSource(this.response.rows);
      this.dataSource = {
        get rows() {
          return ds.rows;
        },
        notifyUpdate(model: DataGridModel) {
          // We override the notifyUpdate method to ignore filters, as the data is
          // assumed to be pre-filtered. We still apply sorting and aggregations.
          const newModel: DataGridModel = {
            ...model,
            filters: [], // Always pass an empty array of filters.
          };
          ds.notifyUpdate(newModel);
        },
      };

      const error = findErrors(this.query, this.response);
      const warning = findWarnings(this.response, node);
      const noDataWarning =
        this.response?.totalRowCount === 0
          ? new Error('Query returned no rows')
          : undefined;

      this.queryExecuted = true;
      if (error || warning || noDataWarning) {
        if (!node.state.issues) {
          node.state.issues = new NodeIssues();
        }
        node.state.issues.queryError = error;
        node.state.issues.responseError = warning;
        node.state.issues.dataError = noDataWarning;
      } else {
        node.state.issues = undefined;
      }

      // Update columns for SQL source nodes and trigger re-analysis
      if (node instanceof SqlSourceNode) {
        node.onQueryExecuted(this.response.columns);
      }

      // Automatically materialize the node after successful execution
      if (isAQuery(this.query) && !error && !warning) {
        try {
          await this.materializationService.materializeNode(node, this.query);
        } catch (e) {
          console.error('Failed to materialize node:', e);
          // Don't block the UI on materialization errors
        }
      }

      // Force re-analysis for SQL source nodes so downstream nodes can see updated columns
      if (node instanceof SqlSourceNode) {
        this.query = undefined;
        this.queryExecuted = false;
      }
    } catch (e) {
      console.error('Failed to run query:', e);
      // Set error state on the node
      if (!node.state.issues) {
        node.state.issues = new NodeIssues();
      }
      node.state.issues.queryError =
        e instanceof Error ? e : new Error(String(e));
    } finally {
      this.isQueryRunning = false;
      m.redraw();
    }
  }
}
