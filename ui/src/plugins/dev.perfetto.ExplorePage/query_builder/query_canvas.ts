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
import {QueryNode} from '../query_node';
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

interface QueryCanvasAttrs {
  readonly rootNodes: QueryNode[];
  readonly selectedNode?: QueryNode;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly renderNodeActionsMenuItems: (node: QueryNode) => m.Children;
  readonly addSourcePopupMenu: () => m.Children;
}

export class QueryCanvas implements m.ClassComponent<QueryCanvasAttrs> {
  private dragNode?: QueryNode;
  private nodeLayouts: Map<QueryNode, NodeBoxLayout> = new Map();

  oncreate(vnode: m.VnodeDOM<QueryCanvasAttrs>) {
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

  view({attrs}: m.CVnode<QueryCanvasAttrs>) {
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
      // Render the centered "Add" button if no nodes exist
      nodes.push(
        m(
          '',
          {
            style: {
              position: 'absolute',
              left: '50%',
              top: '50%',
              transform: 'translate(-50%, -50%)',
            },
          },
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
            addSourcePopupMenu(),
          ),
        ),
      );
    } else {
      rootNodes.forEach((rootNode) => {
        let curNode: QueryNode | undefined = rootNode;
        while (curNode) {
          const localCurNode = curNode;
          const layout = this.nodeLayouts.get(localCurNode) || {x: 10, y: 10};
          if (!this.nodeLayouts.has(localCurNode)) {
            this.nodeLayouts.set(localCurNode, layout);
          }
          nodes.push(
            m(NodeBox, {
              node: localCurNode,
              isSelected: selectedNode === localCurNode,
              layout,
              onNodeSelected,
              renderNodeActionsMenuItems,
              onNodeDragStart: this.onNodeDragStart,
            }),
          );
          curNode = curNode.nextNode;
        }
      });
    }

    return m(
      '.query-canvas-container',
      {
        style: {
          position: 'relative', // Absolute positioning of NodeBoxes
          height: '800px',
          backgroundColor: 'lightgray',
          overflow: 'auto', // Required for scroll functionality
        },
      },
      nodes,
    );
  }
}
