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
import {classNames} from '../../../base/classnames';

import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {QueryNode, Query, isAQuery, queryToRun, NodeType} from '../query_node';
import {ExplorePageHelp} from './help';
import {NodeExplorer} from './node_explorer';
import {Graph} from './graph';
import {Trace} from 'src/public/trace';
import {DataExplorer} from './data_explorer';
import {
  DataGridDataSource,
  DataGridModel,
  FilterDefinition,
} from '../../../components/widgets/data_grid/common';
import {InMemoryDataSource} from '../../../components/widgets/data_grid/in_memory_data_source';
import {QueryResponse} from '../../../components/query_table/queries';
import {TableSourceNode} from './nodes/sources/table_source';
import {SqlSourceNode} from './nodes/sources/sql_source';
import {QueryService} from './query_service';
import {findErrors, findWarnings} from './query_builder_utils';
import {NodeIssues} from './node_issues';
import {NodeBoxLayout} from './node_box';

export interface BuilderAttrs {
  readonly trace: Trace;

  readonly sqlModules: SqlModules;
  readonly rootNodes: QueryNode[];
  readonly selectedNode?: QueryNode;
  readonly nodeLayouts: Map<string, NodeBoxLayout>;

  readonly onRootNodeCreated: (node: QueryNode) => void;
  readonly onNodeSelected: (node?: QueryNode) => void;
  readonly onDeselect: () => void;
  readonly onNodeLayoutChange: (nodeId: string, layout: NodeBoxLayout) => void;

  // Add source nodes.
  readonly onAddStdlibTableSource: () => void;
  readonly onAddSlicesSource: () => void;
  readonly onAddSqlSource: () => void;

  // Add derived nodes.
  readonly onAddAggregationNode: (node: QueryNode) => void;
  readonly onAddModifyColumnsNode: (node: QueryNode) => void;
  readonly onAddIntervalIntersectNode: (node: QueryNode) => void;

  readonly onClearAllNodes: () => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
  readonly onImport: () => void;
  readonly onImportWithStatement: () => void;
  readonly onExport: () => void;
  readonly onRemoveFilter: (node: QueryNode, filter: FilterDefinition) => void;
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
      onAddStdlibTableSource,
      onAddSlicesSource,
      onAddSqlSource,
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
          onQueryAnalyzed: (
            query: Query | Error,
            reexecute = selectedNode.type !== NodeType.kSqlSource &&
              selectedNode.type !== NodeType.kIntervalIntersect,
          ) => {
            this.query = query;
            if (isAQuery(this.query) && reexecute) {
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

            onRootNodeCreated(
              new TableSourceNode({
                trace,
                sqlModules,
                sqlTable,
                filters: [],
              }),
            );
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
          onAddStdlibTableSource,
          onAddSlicesSource,
          onAddSqlSource,
          onClearAllNodes,
          onDuplicateNode: attrs.onDuplicateNode,
          onAddAggregation: attrs.onAddAggregationNode,
          onAddModifyColumns: attrs.onAddModifyColumnsNode,
          onAddIntervalIntersect: attrs.onAddIntervalIntersectNode,
          onDeleteNode: (node: QueryNode) => {
            if (node.isMaterialised()) {
              trace.engine.query(`DROP TABLE IF EXISTS ${node.meterialisedAs}`);
            }
            attrs.onDeleteNode(node);
          },
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
            executeQuery: !this.queryExecuted,
            response: this.response,
            dataSource: this.dataSource,
            onchange: () => {},
            onQueryExecuted: ({
              columns,
              error,
              warning,
              noDataWarning,
            }: {
              columns: string[];
              error?: Error;
              warning?: Error;
              noDataWarning?: Error;
            }) => {
              this.queryExecuted = true;

              if (error || warning || noDataWarning) {
                if (!selectedNode.state.issues) {
                  selectedNode.state.issues = new NodeIssues();
                }
                selectedNode.state.issues.queryError = error;
                selectedNode.state.issues.responseError = warning;
                selectedNode.state.issues.dataError = noDataWarning;
              } else {
                selectedNode.state.issues = undefined;
              }

              if (selectedNode instanceof SqlSourceNode) {
                selectedNode.onQueryExecuted(columns);
              }
            },
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
