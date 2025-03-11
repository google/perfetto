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
import {showModal} from '../../../widgets/modal';
import {DataSourceViewer} from './data_source_viewer';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {
  StdlibTableAttrs,
  StdlibTableNode,
  StdlibTableSource,
} from './sources/stdlib_table';
import {SqlSource, SqlSourceAttrs, SqlSourceNode} from './sources/sql_source';
import {Icons} from '../../../base/semantic_icons';
import {
  SlicesSource,
  SlicesSourceAttrs,
  SlicesSourceNode,
} from './sources/slices_source';
import {Intent} from '../../../widgets/common';

export interface QueryBuilderTable {
  name: string;
  asSqlTable: SqlTable;
  columnOptions: ColumnControllerRow[];
  sql: string;
}

export interface QueryBuilderAttrs extends PageWithTraceAttrs {
  readonly sqlModules: SqlModules;
  readonly rootNode?: QueryNode;
  readonly selectedNode?: QueryNode;

  readonly onRootNodeCreated: (node: QueryNode) => void;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly visualiseDataMenuItems: (node: QueryNode) => m.Children;
}

interface NodeAttrs {
  node: QueryNode;
  isSelected: boolean;
  onNodeSelected: (node: QueryNode) => void;
}

class NodeBox implements m.ClassComponent<NodeAttrs> {
  view({attrs}: m.CVnode<NodeAttrs>) {
    const {node, isSelected, onNodeSelected} = attrs;
    return m(
      '.node-box',
      {
        style: {
          border: isSelected ? '2px solid yellow' : '2px solid blue',
          borderRadius: '5px',
          padding: '10px',
          cursor: 'pointer',
          backgroundColor: 'lightblue',
        },
        onclick: () => onNodeSelected(node),
      },
      node.getTitle(),
    );
  }
}

export class QueryBuilder implements m.ClassComponent<QueryBuilderAttrs> {
  view({attrs}: m.CVnode<QueryBuilderAttrs>) {
    const {
      trace,
      sqlModules,
      rootNode,
      onRootNodeCreated,
      onNodeSelected,
      selectedNode,
    } = attrs;

    const chooseSourceButton = (): m.Child => {
      return m(
        PopupMenu,
        {
          trigger: m(Button, {
            icon: Icons.Add,
            intent: Intent.Primary,
            style: {
              height: '100px',
              width: '100px',
              display: 'flex',
              justifyContent: 'center',
              alignItems: 'center',
              fontSize: '48px',
            },
          }),
        },
        m(MenuItem, {
          label: 'Standard library table',
          onclick: async () => {
            const attrs: StdlibTableAttrs = {
              filters: [],
              sourceCols: [],
              groupByColumns: [],
              aggregations: [],
              trace,
              sqlModules,
              modal: () =>
                createModal(
                  'Standard library table',
                  () => m(StdlibTableSource, attrs),
                  () => {
                    const newNode = new StdlibTableNode(attrs);
                    onRootNodeCreated(newNode);
                    onNodeSelected(newNode);
                  },
                ),
            };
            // Adding trivial modal to open the table selection.
            createModal(
              'Standard library table',
              () => m(StdlibTableSource, attrs),
              () => {},
            );
          },
        }),
        m(MenuItem, {
          label: 'Custom slices',
          onclick: () => {
            const newSimpleSlicesAttrs: SlicesSourceAttrs = {
              sourceCols: [],
              filters: [],
              groupByColumns: [],
              aggregations: [],
            };
            createModal(
              'Slices',
              () => m(SlicesSource, newSimpleSlicesAttrs),
              () => {
                const newNode = new SlicesSourceNode(newSimpleSlicesAttrs);
                onRootNodeCreated(newNode);
                onNodeSelected(newNode);
              },
            );
          },
        }),
        m(MenuItem, {
          label: 'Custom SQL',
          onclick: () => {
            const newSqlSourceAttrs: SqlSourceAttrs = {
              sourceCols: [],
              filters: [],
              groupByColumns: [],
              aggregations: [],
            };
            createModal(
              'SQL',
              () => m(SqlSource, newSqlSourceAttrs),
              () => {
                const newNode = new SqlSourceNode(newSqlSourceAttrs);
                onRootNodeCreated(newNode);
                onNodeSelected(newNode);
              },
            );
          },
        }),
      );
    };

    const renderNodeActions = (curNode: QueryNode) => {
      return m(
        PopupMenu,
        {
          trigger: m(Button, {
            iconFilled: true,
            icon: Icons.MoreVert,
          }),
        },
        attrs.visualiseDataMenuItems(curNode),
      );
    };

    const renderNodesPanel = (): m.Children => {
      const nodes: m.Child[] = [];
      let row = 1;

      if (!rootNode) {
        nodes.push(
          m('', {style: {gridColumn: 3, gridRow: 2}}, chooseSourceButton()),
        );
      } else {
        let curNode: QueryNode | undefined = rootNode;
        while (curNode) {
          const localCurNode = curNode;
          nodes.push(
            m(
              '',
              {style: {display: 'flex', gridColumn: 3, gridRow: row}},
              m(NodeBox, {
                node: localCurNode,
                isSelected: selectedNode === localCurNode,
                onNodeSelected,
              }),
              renderNodeActions(curNode),
            ),
          );
          row++;
          curNode = curNode.nextNode;
        }
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
      return attrs.selectedNode
        ? m(DataSourceViewer, {trace, queryNode: attrs.selectedNode})
        : undefined;
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

export const createModal = (
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
