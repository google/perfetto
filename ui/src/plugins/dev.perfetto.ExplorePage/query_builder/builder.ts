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
import {Button, ButtonVariant} from '../../../widgets/button';
import {SqlModules, SqlTable} from '../../dev.perfetto.SqlModules/sql_modules';
import {ColumnControllerRow} from './column_controller';
import {QueryNode} from '../query_node';
import {showModal} from '../../../widgets/modal';
import {DataSourceViewer} from './data_source_viewer';
import {PopupMenu} from '../../../widgets/menu';
import {Icons} from '../../../base/semantic_icons';
import {Intent} from '../../../widgets/common';

export interface QueryBuilderTable {
  name: string;
  asSqlTable: SqlTable;
  columnOptions: ColumnControllerRow;
  sql: string;
}

export interface QueryBuilderAttrs extends PageWithTraceAttrs {
  readonly sqlModules: SqlModules;
  readonly rootNodes: QueryNode[];
  readonly selectedNode?: QueryNode;

  readonly onRootNodeCreated: (node: QueryNode) => void;
  readonly onNodeSelected: (node?: QueryNode) => void;
  readonly renderNodeActionsMenuItems: (node: QueryNode) => m.Children;
  readonly addSourcePopupMenu: () => m.Children;
}

interface NodeAttrs {
  readonly node: QueryNode;
  isSelected: boolean;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly renderNodeActionsMenuItems: (node: QueryNode) => m.Children;
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
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            iconFilled: true,
            icon: Icons.MoreVert,
          }),
        },
        attrs.renderNodeActionsMenuItems(node),
      ),
    );
  }
}

export class QueryBuilder implements m.ClassComponent<QueryBuilderAttrs> {
  view({attrs}: m.CVnode<QueryBuilderAttrs>) {
    const {
      trace,
      rootNodes,
      onNodeSelected,
      selectedNode,
      renderNodeActionsMenuItems,
    } = attrs;

    const renderNodesPanel = (): m.Children => {
      const nodes: m.Child[] = [];
      const numRoots = rootNodes.length;

      if (numRoots === 0) {
        nodes.push(
          m(
            '',
            {style: {gridColumn: 3, gridRow: 2}},
            m(
              PopupMenu,
              {
                trigger: m(Button, {
                  icon: Icons.Add,
                  intent: Intent.Primary,
                  variant: ButtonVariant.Filled,
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
              attrs.addSourcePopupMenu(),
            ),
          ),
        );
      } else {
        let col = 1;
        rootNodes.forEach((rootNode) => {
          let row = 1;
          let curNode: QueryNode | undefined = rootNode;
          while (curNode) {
            const localCurNode = curNode;
            nodes.push(
              m(
                '',
                {style: {display: 'flex', gridColumn: col, gridRow: row}},
                m(NodeBox, {
                  node: localCurNode,
                  isSelected: selectedNode === localCurNode,
                  onNodeSelected,
                  renderNodeActionsMenuItems,
                }),
              ),
            );
            row++;
            curNode = curNode.nextNode;
          }
          col += 1;
        });
      }

      return m(
        '',
        {
          style: {
            display: 'grid',
            gridTemplateColumns: `repeat(${numRoots} - 1, 1fr)`,
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
