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

import {Button} from '../../../widgets/button';
import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {QueryNode} from '../query_node';
import {showModal} from '../../../widgets/modal';
import {DataSourceViewer} from './data_source_viewer';
import {PopupMenu} from '../../../widgets/menu';
import {Icons} from '../../../base/semantic_icons';
import {Intent} from '../../../widgets/common';

interface NodeBoxLayout {
  x: number;
  y: number;
  width?: number;
  height?: number;
}

interface NodeBoxAttrs {
  readonly node: QueryNode;
  readonly layout: NodeBoxLayout;

  readonly isSelected: boolean;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly onNodeDragStart: (node: QueryNode, event: DragEvent) => void;
  readonly renderNodeActionsMenuItems: (node: QueryNode) => m.Children;
}

class NodeBox implements m.ClassComponent<NodeBoxAttrs> {
  view({attrs}: m.CVnode<NodeBoxAttrs>) {
    const {node, isSelected, layout, onNodeSelected, onNodeDragStart} = attrs;
    return m(
      '.node-box',
      {
        style: {
          border: isSelected ? '2px solid yellow' : '2px solid blue',
          borderRadius: '5px',
          padding: '10px',
          cursor: 'grab',
          backgroundColor: 'lightblue',
          position: 'absolute',
          left: `${layout.x || 10}px`,
          top: `${layout.y || 10}px`,
        },
        onclick: () => onNodeSelected(node),
        draggable: true,
        ondragstart: (event: DragEvent) => onNodeDragStart(node, event),
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

interface QueryFrameAttrs {
  readonly rootNodes: QueryNode[];
  readonly selectedNode?: QueryNode;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly renderNodeActionsMenuItems: (node: QueryNode) => m.Children;
  readonly addSourcePopupMenu: () => m.Children;
}

class QueryFrame implements m.ClassComponent<QueryFrameAttrs> {
  private dragNode?: QueryNode;
  private nodeLayouts: Map<QueryNode, NodeBoxLayout> = new Map();

  oncreate(vnode: m.VnodeDOM<QueryFrameAttrs>) {
    const box = vnode.dom as HTMLElement;

    box.ondragover = (event) => {
      event.preventDefault(); // Allow dropping
    };

    box.ondrop = (event) => {
      event.preventDefault();
      if (!this.dragNode) return;
      const dragNodeLayout = this.nodeLayouts.get(this.dragNode);
      if (!dragNodeLayout) return;
      // Adjust position based on where the mouse dropped relative to the box origin
      // and center the node based on its stored dimensions.
      const rect = box.getBoundingClientRect();
      const x = event.clientX - rect.left - (dragNodeLayout.width ?? 50) / 2;
      const y = event.clientY - rect.top - (dragNodeLayout.height ?? 50) / 2;

      this.nodeLayouts.set(this.dragNode, {
        ...dragNodeLayout,
        x: Math.max(0, Math.min(x, rect.width - (dragNodeLayout.width ?? 100))),
        y: Math.max(
          0,
          Math.min(y, rect.height - (dragNodeLayout.height ?? 100)),
        ),
      });
      this.dragNode = undefined;
      m.redraw();
    };
  }

  onNodeDragStart = (node: QueryNode, event: DragEvent) => {
    this.dragNode = node;
    const nodeElem = (event.target as HTMLElement).closest(
      '.node-box',
    ) as HTMLElement;

    const layout = this.nodeLayouts.get(node) || {x: 10, y: 10};
    this.nodeLayouts.set(node, {
      ...layout,
      width: nodeElem.offsetWidth,
      height: nodeElem.offsetHeight,
    });

    if (event.dataTransfer) {
      event.dataTransfer.setData('text/plain', node.getTitle());
      event.dataTransfer.effectAllowed = 'move';
    }
  };

  view({attrs}: m.CVnode<QueryFrameAttrs>) {
    const {
      rootNodes,
      onNodeSelected,
      selectedNode,
      renderNodeActionsMenuItems,
      addSourcePopupMenu,
    } = attrs;

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
      '.query-canvas-container',
      {
        style: {
          position: 'relative', // Absolute positioning of NodeBoxes
          height: '800px',
          border: '1px solid lightgray',
          overflow: 'auto', // Required for scroll functionality
        },
      },
      nodes,
    );
  }
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

export class QueryBuilder implements m.ClassComponent<QueryBuilderAttrs> {
  view({attrs}: m.CVnode<QueryBuilderAttrs>) {
    const {
      trace,
      rootNodes,
      onNodeSelected,
      selectedNode,
      renderNodeActionsMenuItems,
      addSourcePopupMenu,
    } = attrs;

    const renderDataSourceViewer = () => {
      return attrs.selectedNode
        ? m(DataSourceViewer, {trace, queryNode: attrs.selectedNode})
        : undefined;
    };

    return m(
      '.query-builder-layout',
      {
        style: {
          display: 'grid',
          gridTemplateColumns: '50% 50%',
          gridTemplateRows: '1fr auto',
          gap: '10px',
          height: '100%',
        },
      },
      m(
        '',
        {style: {gridColumn: 1, gridRow: 1}},
        m(QueryFrame, {
          rootNodes,
          selectedNode,
          onNodeSelected,
          renderNodeActionsMenuItems,
          addSourcePopupMenu,
        }),
      ),
      m('', {style: {gridColumn: 2, gridRow: 1}}, renderDataSourceViewer()),
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
