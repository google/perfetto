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
import {QueryNode, NodeType, Query, isAQuery} from '../query_node';
import {ExplorePageHelp} from './explore_page_help';
import {QueryNodeExplorer} from './query_node_explorer';
import {NodeGraph} from './node_graph';
import {Trace} from 'src/public/trace';
import {NodeDataViewer} from './node_data_viewer';
import {FilterDefinition} from '../../../components/widgets/data_grid/common';
import {columnInfoFromSqlColumn, newColumnInfoList} from './column_info';
import {StdlibTableNode} from './sources/stdlib_table';
import {SqlSourceNode} from './sources/sql_source';

export interface QueryBuilderAttrs {
  readonly trace: Trace;

  readonly sqlModules: SqlModules;
  readonly rootNodes: QueryNode[];
  readonly selectedNode?: QueryNode;

  readonly onRootNodeCreated: (node: QueryNode) => void;
  readonly onNodeSelected: (node?: QueryNode) => void;
  readonly onDeselect: () => void;
  readonly onAddStdlibTableSource: () => void;
  readonly onAddSlicesSource: () => void;
  readonly onAddSqlSource: () => void;
  readonly onClearAllNodes: () => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
}

export class QueryBuilder implements m.ClassComponent<QueryBuilderAttrs> {
  private query?: Query | Error;
  private queryExecuted: boolean = false;
  private tablePosition: 'left' | 'right' | 'bottom' = 'bottom';
  private previousSelectedNode?: QueryNode;

  view({attrs}: m.CVnode<QueryBuilderAttrs>) {
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
      if (selectedNode.type === NodeType.kSqlSource) {
        this.tablePosition = 'left';
      } else {
        this.tablePosition = 'bottom';
      }
    }
    this.previousSelectedNode = selectedNode;

    const layoutClasses =
      classNames(
        'pf-query-builder-layout',
        selectedNode ? 'selection' : 'no-selection',
        selectedNode && `selection-${this.tablePosition}`,
      ) || '';

    const explorer = selectedNode
      ? m(QueryNodeExplorer, {
          trace,
          node: selectedNode,
          onQueryAnalyzed: (query: Query | Error, reexecute = true) => {
            this.query = query;
            if (isAQuery(this.query) && reexecute) {
              this.queryExecuted = false;
            }
          },
          onExecute: () => {
            this.queryExecuted = false;
            m.redraw();
          },
        })
      : m(ExplorePageHelp, {
          sqlModules,
          onTableClick: (tableName: string) => {
            const {onRootNodeCreated} = attrs;
            const sqlTable = sqlModules.getTable(tableName);
            if (!sqlTable) return;

            const sourceCols = sqlTable.columns.map((c) =>
              columnInfoFromSqlColumn(c, true),
            );
            const groupByColumns = newColumnInfoList(sourceCols, false);

            onRootNodeCreated(
              new StdlibTableNode({
                trace,
                sqlModules,
                sqlTable,
                sourceCols,
                groupByColumns,
                filters: [],
                aggregations: [],
              }),
            );
          },
        });

    return m(
      `.${layoutClasses.split(' ').join('.')}`,
      m(
        '.pf-qb-node-graph',
        m(NodeGraph, {
          rootNodes,
          selectedNode,
          onNodeSelected,
          onDeselect: attrs.onDeselect,
          onAddStdlibTableSource,
          onAddSlicesSource,
          onAddSqlSource,
          onClearAllNodes,
          onDuplicateNode: attrs.onDuplicateNode,
          onDeleteNode: attrs.onDeleteNode,
        }),
      ),
      m('.pf-qb-explorer', explorer),
      selectedNode &&
        m(
          '.pf-qb-viewer',
          m(NodeDataViewer, {
            trace,
            query: this.query,
            executeQuery: !this.queryExecuted,
            filters:
              // TODO(mayzner): This is a temporary fix for handling the filtering of SQL node.
              selectedNode.type === NodeType.kSqlSource
                ? []
                : selectedNode.state.filters,
            onFiltersChanged:
              selectedNode.type === NodeType.kSqlSource
                ? undefined
                : (filters: ReadonlyArray<FilterDefinition>) => {
                    selectedNode.state.filters = filters as FilterDefinition[];
                    this.queryExecuted = false;
                    m.redraw();
                  },
            onQueryExecuted: ({
              columns,
              queryError,
              responseError,
              dataError,
            }: {
              columns: string[];
              queryError?: Error;
              responseError?: Error;
              dataError?: Error;
            }) => {
              this.queryExecuted = true;

              selectedNode.state.queryError = queryError;
              selectedNode.state.responseError = responseError;
              selectedNode.state.dataError = dataError;

              if (selectedNode instanceof SqlSourceNode) {
                selectedNode.setSourceColumns(columns);
              }
            },
            onPositionChange: (pos: 'left' | 'right' | 'bottom') => {
              this.tablePosition = pos;
            },
          }),
        ),
    );
  }
}
