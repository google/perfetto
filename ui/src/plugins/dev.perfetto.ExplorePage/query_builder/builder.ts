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

import {PageWithTraceAttrs} from '../../../public/page';
import {Button} from '../../../widgets/button';
import {SqlModules, SqlTable} from '../../dev.perfetto.SqlModules/sql_modules';
import {ColumnControllerRow} from './column_controller';
import {QueryNode} from '../query_node';
import {JoinState, QueryBuilderJoin} from './operations/join';
import {Intent} from '../../../widgets/common';
import {showModal} from '../../../widgets/modal';
import {DataSourceViewer} from './data_source_viewer';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {getLastFinishedNode} from '../query_node';
import {StdlibTableNode} from './sources/stdlib_table';
import {
  GroupByAttrs,
  GroupByNode,
  GroupByOperation,
} from './operations/groupy_by';
import {FilterAttrs, FilterNode, FilterOperation} from './operations/filter';
import {SqlSource, SqlSourceAttrs, SqlSourceNode} from './sources/sql_source';
import {
  SlicesSourceAttrs,
  SlicesSourceNode,
  SlicesSource,
} from './sources/slices';

export interface QueryBuilderTable {
  name: string;
  asSqlTable: SqlTable;
  columnOptions: ColumnControllerRow[];
  sql: string;
}

export interface QueryBuilderAttrs extends PageWithTraceAttrs {
  readonly sqlModules: SqlModules;
  readonly rootNode?: QueryNode;
  readonly onRootNodeCreated: (node: QueryNode) => void;
}

export class QueryBuilder implements m.ClassComponent<QueryBuilderAttrs> {
  selectedNode?: QueryNode;

  // Method to update the selected node and trigger a redraw.
  selectNode(node: QueryNode) {
    this.selectedNode = node;
  }

  view({attrs}: m.CVnode<QueryBuilderAttrs>) {
    const {trace, sqlModules, rootNode, onRootNodeCreated} = attrs;

    const createModal = (
      title: string,
      content: () => m.Children,
      onAdd: () => void,
    ) => {
      showModal({
        title,
        buttons: [{text: 'Add node', action: onAdd}],
        content,
      });
    };

    const chooseSourceButton = (): m.Child => {
      return m(
        PopupMenu,
        {
          trigger: m(Button, {
            label: 'Choose a source',
            intent: Intent.Primary,
          }),
        },
        m(MenuItem, {
          label: 'Table',
          onclick: async () => {
            const tableName = await trace.omnibox.prompt(
              'Choose a table...',
              sqlModules.listTablesNames(),
            );
            if (!tableName) return;

            const sqlTable = sqlModules.getTable(tableName);
            if (!sqlTable) return;

            const newNode = new StdlibTableNode(sqlTable);
            onRootNodeCreated(newNode);
            this.selectNode(newNode);
          },
        }),
        m(MenuItem, {
          label: 'From custom slices',
          onclick: () => {
            const newSimpleSlicesAttrs: SlicesSourceAttrs = {};
            createModal('Slices', () => m(SlicesSource, newSimpleSlicesAttrs), () => {
              const newNode = new SlicesSourceNode(newSimpleSlicesAttrs);
              onRootNodeCreated(newNode);
              this.selectNode(newNode);
            });
          },
        }),
        m(MenuItem, {
          label: 'From custom SQL',
          onclick: () => {
            const newSqlSourceAttrs: SqlSourceAttrs = {};
            createModal('SQL', () => m(SqlSource, newSqlSourceAttrs), () => {
              const newNode = new SqlSourceNode(newSqlSourceAttrs);
              onRootNodeCreated(newNode);
              this.selectNode(newNode);
            });
          },
        }),
      );
    };

    const chooseOperationButton = (): m.Child => {
      return m(
        PopupMenu,
        {trigger: m(Button, {label: '+', intent: Intent.Primary})},
        m(MenuItem, {
          label: 'GROUP BY',
          onclick: () => {
            if (!rootNode) return;
            const curNode = getLastFinishedNode(rootNode);
            if (!curNode) return;

            const newGroupByAttrs: GroupByAttrs = {prevNode: curNode};
            createModal(
              'GROUP BY',
              () => m(GroupByOperation, newGroupByAttrs),
              () => {
                const newNode = new GroupByNode(newGroupByAttrs);
                curNode.nextNode = newNode;
                this.selectNode(newNode);
              },
            );
          },
        }),
        m(MenuItem, {
          label: 'FILTER',
          onclick: () => {
            if (!rootNode) return;
            const curNode = getLastFinishedNode(rootNode);
            if (!curNode) return;
            const newFilterAttrs: FilterAttrs = {prevNode: curNode};
            createModal('FILTER', () => m(FilterOperation, newFilterAttrs), () => {
              const newNode = new FilterNode(newFilterAttrs);
              curNode.nextNode = newNode;
              this.selectNode(newNode);
            });
          },
        }),
        m(MenuItem, {
          label: 'JOIN',
          disabled: true,
          onclick: () => {
            if (!rootNode) return;
            const curNode = getLastFinishedNode(rootNode);
            if (!curNode) return;

            const newJoinState = new JoinState(curNode);
            createModal(
              'JOIN',
              () => m(QueryBuilderJoin, {sqlModules, joinState: newJoinState}),
              () => {
                newJoinState.validate();
                curNode.nextNode = newJoinState;
                this.selectNode(newJoinState);
              },
            );
          },
        }),
      );
    };

    const renderNodesPanel = (): m.Children => {
      const nodes: m.Child[] = [];
      let row = 1;

      if (!rootNode) {
        nodes.push(
          m('', {style: {gridColumn: 3, gridRow: row}}, chooseSourceButton()),
        );
      } else {
        let curNode: QueryNode | undefined = rootNode;
        while (curNode) {
          const localCurNode = curNode;
          nodes.push(
            m(
              '',
              {style: {gridColumn: 3, gridRow: row}},
              m(Button, {
                label: localCurNode.getTitle(),
                intent: Intent.Primary,
                onclick: () => this.selectNode(localCurNode),
              }),
            ),
          );
          row++;
          curNode = curNode.nextNode;
        }

        nodes.push(
          m(
            '',
            {style: {gridColumn: 3, gridRow: row}},
            chooseOperationButton(),
          ),
        );
      }

      return m(
        '',
        {
          style: {
            display: 'grid',
            gridTemplateColumns: 'repeat(5, 1fr)',
            gridTemplateRows: 'repeat(3, 1fr)',
            gap: '10px',
          },
        },
        nodes,
      );
    };

    const renderDataSourceViewer = () => {
      if (!this.selectedNode) return;
      return m(DataSourceViewer, {trace, queryNode: this.selectedNode});
    };

    return m(
      '',
      {
        style: {
          display: 'grid',
          gridTemplateColumns: '50% 50%',
          gridTemplateRows: '50% 50%',
          gap: '10px',
        },
      },
      m('', {style: {gridColumn: 1}}, renderNodesPanel()),
      m('', {style: {gridColumn: 2}}, renderDataSourceViewer()),
    );
  }
}
