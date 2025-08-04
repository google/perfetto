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
import {ExplorePageHelp} from './help';
import {NodeExplorer} from './node_explorer';
import {Graph} from './graph';
import {Trace} from 'src/public/trace';
import {DataExplorer} from './data_explorer';
import {columnInfoFromSqlColumn, newColumnInfoList} from './column_info';
import {TableSourceNode} from './sources/table_source';
import {SqlSourceNode} from './sources/sql_source';

export interface BuilderAttrs {
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

export class Builder implements m.ClassComponent<BuilderAttrs> {
  private query?: Query | Error;
  private queryExecuted: boolean = false;
  private tablePosition: 'left' | 'right' | 'bottom' = 'bottom';
  private previousSelectedNode?: QueryNode;
  private isNodeDataViewerFullScreen: boolean = false;

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
          onchange: () => {
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
              new TableSourceNode({
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
        m(Graph, {
          rootNodes,
          selectedNode,
          onNodeSelected,
          onDeselect: attrs.onDeselect,
          onAddStdlibTableSource,
          onAddSlicesSource,
          onAddSqlSource,
          onClearAllNodes,
          onDuplicateNode: attrs.onDuplicateNode,
          onDeleteNode: (node: QueryNode) => {
            if (node.state.isExecuted && node.graphTableName) {
              trace.engine.query(`DROP TABLE IF EXISTS ${node.graphTableName}`);
            }
            attrs.onDeleteNode(node);
          },
        }),
      ),
      m('.pf-qb-explorer', explorer),
      selectedNode &&
        m(
          '.pf-qb-viewer',
          m(DataExplorer, {
            trace,
            query: this.query,
            node: selectedNode,
            executeQuery: !this.queryExecuted,
            onchange: () => {
              this.query = undefined;
              this.queryExecuted = false;
              m.redraw();
            },
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

              selectedNode.state.queryError = error;
              selectedNode.state.responseError = warning;
              selectedNode.state.dataError = noDataWarning;

              if (selectedNode instanceof SqlSourceNode) {
                selectedNode.setSourceColumns(columns);
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
}
