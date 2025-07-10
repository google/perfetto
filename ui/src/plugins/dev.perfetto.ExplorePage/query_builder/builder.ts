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

import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {QueryNode, NodeType} from '../query_node';
import {ExplorePageHelp} from './explore_page_help';
import {Query, QueryNodeExplorer} from './query_node_explorer';
import {QueryCanvas} from './query_canvas';
import {Trace} from 'src/public/trace';
import {NodeDataViewer} from './node_data_viewer';
import {columnInfoFromSqlColumn, newColumnInfoList} from './column_info';
import {StdlibTableNode} from './sources/stdlib_table';

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
  readonly onVisualizeNode: (node: QueryNode) => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
}

export class QueryBuilder implements m.ClassComponent<QueryBuilderAttrs> {
  private query?: Query;
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

    const layoutStyle: m.Attributes['style'] = {
      display: 'grid',
      gridTemplateColumns: '50% 50%',
      gridTemplateRows: selectedNode ? '50% 50%' : 'auto 1fr',
      gap: '10px',
      height: '100%',
    };

    const canvasStyle: m.Attributes['style'] = {
      gridColumn: 1,
      gridRow: 1,
      overflow: 'auto',
    };
    const explorerStyle: m.Attributes['style'] = {
      gridColumn: 2,
      gridRow: 1,
      overflow: 'auto',
    };
    const viewerStyle: m.Attributes['style'] = {
      overflow: 'auto',
    };

    if (selectedNode) {
      switch (this.tablePosition) {
        case 'left':
          viewerStyle.gridColumn = 1;
          viewerStyle.gridRow = 2;
          explorerStyle.gridRow = '1 / span 2';
          break;
        case 'right':
          viewerStyle.gridColumn = 2;
          viewerStyle.gridRow = 2;
          canvasStyle.gridRow = '1 / span 2';
          break;
        case 'bottom':
          viewerStyle.gridColumn = '1 / span 2';
          viewerStyle.gridRow = 2;
          break;
      }
    }

    const explorer = selectedNode
      ? m(QueryNodeExplorer, {
          trace,
          node: selectedNode,
          onQueryAnalyzed: (query: Query) => {
            this.query = query;
            this.queryExecuted = false;
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
      '.query-builder-layout',
      {
        style: layoutStyle,
      },
      m(
        'div',
        {style: canvasStyle},
        m(QueryCanvas, {
          rootNodes,
          selectedNode,
          onNodeSelected,
          onDeselect: attrs.onDeselect,
          onAddStdlibTableSource,
          onAddSlicesSource,
          onAddSqlSource,
          onClearAllNodes,
          onVisualizeNode: attrs.onVisualizeNode,
          onDuplicateNode: attrs.onDuplicateNode,
          onDeleteNode: attrs.onDeleteNode,
        }),
      ),
      m('div', {style: explorerStyle}, explorer),
      selectedNode &&
        m(
          'div',
          {style: viewerStyle},
          m(NodeDataViewer, {
            trace,
            query: this.query,
            executeQuery: !this.queryExecuted,
            onQueryExecuted: () => {
              this.queryExecuted = true;
            },
            onPositionChange: (pos: 'left' | 'right' | 'bottom') => {
              this.tablePosition = pos;
            },
          }),
        ),
    );
  }
}
