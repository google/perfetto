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

import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {QueryNode, Query, isAQuery, queryToRun} from '../query_node';
import {ExplorePageHelp} from './help';
import {NodeExplorer} from './node_explorer';
import {Graph} from './graph/graph';
import {Trace} from 'src/public/trace';
import {DataExplorer} from './data_explorer';
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
  readonly onRemoveFilter: (node: QueryNode, filter: UIFilter) => void;

  // Import / Export JSON
  readonly onImport: () => void;
  readonly onExport: () => void;

  readonly onImportWithStatement: () => void;
}

export class Builder implements m.ClassComponent<BuilderAttrs> {
  private queryService: QueryService;
  private query?: Query | Error;
  private queryExecuted: boolean = false;
  private tablePosition: 'left' | 'right' | 'bottom' = 'bottom';
  private previousSelectedNode?: QueryNode;
  private isNodeDataViewerFullScreen: boolean = false;
  private response?: QueryResponse;
  private dataSource?: DataGridDataSource;

  constructor({attrs}: m.Vnode<BuilderAttrs>) {
    this.queryService = new QueryService(attrs.trace.engine);
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
      if (selectedNode instanceof SqlSourceNode) {
        this.tablePosition = 'left';
      } else {
        this.tablePosition = 'bottom';
      }
      this.response = undefined;
      this.dataSource = undefined;
    }
    this.previousSelectedNode = selectedNode;

    const layoutClasses =
      classNames(
        'pf-query-builder-layout',
        selectedNode ? 'selection' : 'no-selection',
        selectedNode && `selection-${this.tablePosition}`,
        this.isNodeDataViewerFullScreen && 'full-page',
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
            const shouldAutoExecute = selectedNode.state.autoExecute ?? true;
            if (isAQuery(this.query) && shouldAutoExecute) {
              this.queryExecuted = false;
              this.runQuery(selectedNode);
            }
          },
          onExecute: () => {
            this.queryExecuted = false;
            this.runQuery(selectedNode);
            m.redraw();
          },
          onchange: () => {},
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
      `.${layoutClasses.split(' ').join('.')}`,
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
          onImport: attrs.onImport,
          onImportWithStatement: attrs.onImportWithStatement,
          onExport: attrs.onExport,
          onRemoveFilter: attrs.onRemoveFilter,
        }),
      ),
      m('.pf-qb-explorer', explorer),
      selectedNode &&
        m(
          '.pf-qb-viewer',
          m(DataExplorer, {
            queryService: this.queryService,
            query: this.query,
            node: selectedNode,
            response: this.response,
            dataSource: this.dataSource,
            onchange: () => {},
            onPositionChange: (pos: 'left' | 'right' | 'bottom') => {
              this.tablePosition = pos;
            },
            isFullScreen: this.isNodeDataViewerFullScreen,
            onFullScreenToggle: () => {
              this.isNodeDataViewerFullScreen =
                !this.isNodeDataViewerFullScreen;
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

  private runQuery(node: QueryNode) {
    if (
      this.query === undefined ||
      this.query instanceof Error ||
      this.queryExecuted
    ) {
      return;
    }

    this.queryService.runQuery(queryToRun(this.query)).then((response) => {
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

      if (node instanceof SqlSourceNode) {
        node.onQueryExecuted(this.response.columns);
      }
      m.redraw();
    });
  }
}
