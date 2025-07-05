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
import {Icons} from '../../../base/semantic_icons';
import {Button} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {QueryNode} from '../query_node';

const PADDING = 20;
const NODE_HEIGHT = 50;
const DEFAULT_NODE_WIDTH = 100;

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
  readonly isDragging: boolean;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly onNodeDragStart: (node: QueryNode, event: DragEvent) => void;
  readonly onVisualizeNode: (node: QueryNode) => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
}

const NodeBox: m.Component<NodeBoxAttrs> = {
  view({attrs}) {
    const {
      node,
      layout,
      isSelected,
      isDragging,
      onNodeSelected,
      onNodeDragStart,
      onVisualizeNode,
      onDuplicateNode,
      onDeleteNode,
    } = attrs;
    const conditionalClasses = classNames(
      isSelected && 'pf-node-box__selected',
      !node.validate() && 'pf-node-box__invalid',
    );
    return m(
      '.pf-node-box',
      {
        class: conditionalClasses,
        style: {
          position: 'absolute',
          left: `${layout.x}px`,
          top: `${layout.y}px`,
          opacity: isDragging ? '0' : '1',
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
            icon: Icons.ContextMenuAlt,
          }),
        },
        m(MenuItem, {
          label: 'Visualise Data',
          icon: Icons.Chart,
          onclick: () => onVisualizeNode(node),
        }),
        m(MenuItem, {
          label: 'Duplicate',
          onclick: () => onDuplicateNode(node),
        }),
        m(MenuItem, {
          label: 'Delete',
          onclick: () => onDeleteNode(node),
        }),
      ),
    );
  },
};

interface SourceCardAttrs {
  title: string;
  description: string;
  icon: string;
  onclick: () => void;
}

const SourceCard: m.Component<SourceCardAttrs> = {
  view({attrs}) {
    const {title, description, icon, onclick} = attrs;
    return m(
      'div.pf-source-card',
      {onclick},
      m('i.material-icons', icon),
      m('h3', title),
      m('p', description),
    );
  },
};
export interface QueryCanvasAttrs {
  readonly rootNodes: QueryNode[];
  readonly selectedNode?: QueryNode;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly onDeselect: () => void;
  readonly onAddStdlibTableSource: () => void;
  readonly onAddSlicesSource: () => void;
  readonly onAddSqlSource: () => void;
  readonly onClearAllNodes: () => void;
  readonly onVisualizeNode: (node: QueryNode) => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
}

export class QueryCanvas implements m.ClassComponent<QueryCanvasAttrs> {
  private dragNode?: QueryNode;
  private nodeLayouts: Map<QueryNode, NodeBoxLayout> = new Map();

  oncreate({dom}: m.VnodeDOM<QueryCanvasAttrs>) {
    const box = dom as HTMLElement;

    box.ondragover = (event) => {
      event.preventDefault(); // Allow dropping
    };

    box.ondrop = (event) => {
      event.preventDefault();
      if (!this.dragNode) return;
      const dragNodeLayout = this.nodeLayouts.get(this.dragNode);
      if (!dragNodeLayout) return;

      const rect = box.getBoundingClientRect();
      const x =
        event.clientX -
        rect.left -
        (dragNodeLayout.width ?? DEFAULT_NODE_WIDTH) / 2;
      const y =
        event.clientY - rect.top - (dragNodeLayout.height ?? NODE_HEIGHT) / 2;

      this.nodeLayouts.set(this.dragNode, {
        ...dragNodeLayout,
        x: Math.max(
          0,
          Math.min(
            x,
            rect.width - (dragNodeLayout.width ?? DEFAULT_NODE_WIDTH),
          ),
        ),
        y: Math.max(
          0,
          Math.min(y, rect.height - (dragNodeLayout.height ?? NODE_HEIGHT)),
        ),
      });
      m.redraw();
    };

    box.ondragend = () => {
      if (this.dragNode) {
        this.dragNode = undefined;
        m.redraw();
      }
    };
  }

  onNodeDragStart = (node: QueryNode, event: DragEvent) => {
    this.dragNode = node;
    const nodeElem = (event.target as HTMLElement).closest(
      '.pf-node-box',
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

  findNextAvailablePosition(): NodeBoxLayout {
    let y = 10;
    const layouts = Array.from(this.nodeLayouts.values());
    if (layouts.length > 0) {
      const lastNode = layouts.reduce((a, b) => (a.y > b.y ? a : b));
      y = lastNode.y + (lastNode.height ?? NODE_HEIGHT) + PADDING;
    }
    return {x: 10, y};
  }

  private renderEmptyCanvas(attrs: QueryCanvasAttrs) {
    return m(
      '.pf-query-canvas-add-button-container',
      m('h2.hero-title', 'Welcome to the Explore Page'),
      m(
        'p.hero-subtitle',
        'Build and execute SQL queries on your trace data using a visual ' +
          'node-based editor. Get started by adding a source node below.',
      ),
      m(
        '.pf-query-canvas-add-buttons',
        m(SourceCard, {
          title: 'Standard Library Table',
          description:
            'Query and explore data from any table in the Perfetto ' +
            'standard library.',
          icon: 'table_chart',
          onclick: attrs.onAddStdlibTableSource,
        }),
        m(SourceCard, {
          title: 'Slices',
          description: 'Explore all the slices from your trace.',
          icon: 'bar_chart',
          onclick: attrs.onAddSlicesSource,
        }),
        m(SourceCard, {
          title: 'SQL Query',
          description:
            'Start with a custom SQL query to act as a source for ' +
            'further exploration.',
          icon: 'code',
          onclick: attrs.onAddSqlSource,
        }),
      ),
    );
  }

  private renderControls(attrs: QueryCanvasAttrs) {
    return m(
      '.pf-query-canvas__controls',
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            label: 'Add Node',
            icon: Icons.Add,
            intent: Intent.Primary,
          }),
        },
        m(MenuItem, {
          label: 'Explore table',
          onclick: attrs.onAddStdlibTableSource,
        }),
        m(MenuItem, {
          label: 'Explore slices',
          onclick: attrs.onAddSlicesSource,
        }),
        m(MenuItem, {
          label: 'Query',
          onclick: attrs.onAddSqlSource,
        }),
      ),
      m(Button, {
        label: 'Clear All Nodes',
        icon: Icons.Delete,
        intent: Intent.Danger,
        onclick: attrs.onClearAllNodes,
        style: {marginLeft: '8px'},
      }),
    );
  }

  view({attrs}: m.CVnode<QueryCanvasAttrs>) {
    const {rootNodes, onNodeSelected, selectedNode} = attrs;

    const allNodes: QueryNode[] = [];
    for (const root of rootNodes) {
      let curr: QueryNode | undefined = root;
      while (curr) {
        allNodes.push(curr);
        curr = curr.nextNode;
      }
    }

    const children: m.Child[] = [];

    if (allNodes.length === 0) {
      children.push(this.renderEmptyCanvas(attrs));
    } else {
      for (const node of allNodes) {
        let layout = this.nodeLayouts.get(node);
        if (!layout) {
          layout = this.findNextAvailablePosition();
          this.nodeLayouts.set(node, layout);
        }
        children.push(
          m(NodeBox, {
            node,
            isSelected: selectedNode === node,
            isDragging: this.dragNode === node,
            layout,
            onNodeSelected,
            onNodeDragStart: this.onNodeDragStart,
            onVisualizeNode: attrs.onVisualizeNode,
            onDuplicateNode: attrs.onDuplicateNode,
            onDeleteNode: attrs.onDeleteNode,
          }),
        );
      }
      children.push(this.renderControls(attrs));
    }

    return m(
      '.pf-query-canvas',
      {
        tabindex: 0,
        onclick: (e: MouseEvent) => {
          if (e.target === e.currentTarget) {
            attrs.onDeselect();
          }
        },
      },
      children,
    );
  }
}
