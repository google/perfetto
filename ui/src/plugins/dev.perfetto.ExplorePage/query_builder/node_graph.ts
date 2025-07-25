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

import {Icons} from '../../../base/semantic_icons';
import {Button} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {QueryNode} from '../query_node';

import {
  NodeBox,
  NodeBoxLayout,
  NODE_HEIGHT,
  PADDING,
  DEFAULT_NODE_WIDTH,
} from './node_box';

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
      '.pf-source-card',
      {onclick},
      m('i.material-icons', icon),
      m('h3', title),
      m('p', description),
    );
  },
};
export interface NodeGraphAttrs {
  readonly rootNodes: QueryNode[];
  readonly selectedNode?: QueryNode;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly onDeselect: () => void;
  readonly onAddStdlibTableSource: () => void;
  readonly onAddSlicesSource: () => void;
  readonly onAddSqlSource: () => void;
  readonly onClearAllNodes: () => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
}

export class NodeGraph implements m.ClassComponent<NodeGraphAttrs> {
  private dragNode?: QueryNode;
  private nodeLayouts: Map<QueryNode, NodeBoxLayout> = new Map();
  private nodeGraphWidth: number = 0;

  oncreate({dom}: m.VnodeDOM<NodeGraphAttrs>) {
    const box = dom as HTMLElement;
    this.nodeGraphWidth = box.getBoundingClientRect().width;

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

  onNodeRendered = (node: QueryNode, element: HTMLElement) => {
    const layout = this.nodeLayouts.get(node);
    if (layout) {
      const newWidth = element.offsetWidth;
      const newHeight = element.offsetHeight;
      if (layout.width !== newWidth || layout.height !== newHeight) {
        this.nodeLayouts.set(node, {
          ...layout,
          width: newWidth,
          height: newHeight,
        });
      }
    }
  };

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

  private renderEmptyNodeGraph(attrs: NodeGraphAttrs) {
    return m(
      '.pf-node-graph-add-button-container.pf-hero',
      m('h2.hero-title', 'Welcome to the Explore Page'),
      m(
        'p.hero-subtitle',
        'Build and execute SQL queries on your trace data using a visual ' +
          'node-based editor. Get started by adding a source node below.',
      ),
      m(
        '.pf-node-graph-add-buttons',
        m(SourceCard, {
          title: 'Perfetto Table',
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
          title: 'Query Node',
          description:
            'Start with a custom SQL query to act as a source for ' +
            'further exploration.',
          icon: 'code',
          onclick: attrs.onAddSqlSource,
        }),
      ),
    );
  }

  private renderControls(attrs: NodeGraphAttrs) {
    return m(
      '.pf-node-graph__controls',
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
          label: 'Query Node',
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

  view({attrs}: m.CVnode<NodeGraphAttrs>) {
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
      children.push(this.renderEmptyNodeGraph(attrs));
    } else {
      for (const node of allNodes) {
        let layout = this.nodeLayouts.get(node);
        if (!layout) {
          layout = findNextAvailablePosition(
            Array.from(this.nodeLayouts.values()),
            this.nodeGraphWidth,
          );
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
            onDuplicateNode: attrs.onDuplicateNode,
            onDeleteNode: attrs.onDeleteNode,
            onNodeRendered: this.onNodeRendered,
          }),
        );
      }
      children.push(this.renderControls(attrs));
    }

    return m(
      '.pf-node-graph',
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

function findNextAvailablePosition(
  layouts: NodeBoxLayout[],
  nodeGraphWidth: number,
): NodeBoxLayout {
  const w = DEFAULT_NODE_WIDTH;
  const h = NODE_HEIGHT;

  const candidates: {x: number; y: number}[] = [{x: PADDING, y: PADDING}];

  for (const layout of layouts) {
    candidates.push({x: layout.x + (layout.width ?? w) + PADDING, y: layout.y});
    candidates.push({
      x: layout.x,
      y: layout.y + (layout.height ?? h) + PADDING,
    });
  }

  const sortedCandidates = candidates
    .filter((p) => p.x + w <= nodeGraphWidth)
    .sort((a, b) => a.y - b.y || a.x - b.x);

  for (const candidate of sortedCandidates) {
    let isInvalid = false;
    for (const layout of layouts) {
      const layoutW = layout.width ?? w;
      const layoutH = layout.height ?? h;
      if (
        candidate.x < layout.x + layoutW &&
        candidate.x + w > layout.x &&
        candidate.y < layout.y + layoutH &&
        candidate.y + h > layout.y
      ) {
        isInvalid = true;
        break;
      }
    }
    if (!isInvalid) {
      return candidate;
    }
  }

  // Fallback if no candidates are valid (e.g. nodeGraph is full)
  if (layouts.length === 0) return {x: PADDING, y: PADDING};
  const lastNode = layouts.reduce((a, b) => (a.y > b.y ? a : b));
  return {x: PADDING, y: lastNode.y + (lastNode.height ?? h) + PADDING};
}
