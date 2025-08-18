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
import {Icon} from '../../../widgets/icon';

const BUTTONS_AREA_WIDTH = 300;
const BUTTONS_AREA_HEIGHT = 50;

function keycap(glyph: m.Children): m.Children {
  return m('.pf-keycap', glyph);
}

interface SourceCardAttrs {
  title: string;
  description: string;
  icon: string;
  hotkey: string;
  onclick: () => void;
}

const SourceCard: m.Component<SourceCardAttrs> = {
  view({attrs}) {
    const {title, description, icon, hotkey, onclick} = attrs;
    return m(
      '.pf-source-card',
      {onclick},
      m('.pf-source-card-clickable', m(Icon, {icon}), m('h3', title)),
      m('p', description),
      m('.pf-source-card-hotkey', keycap(hotkey)),
    );
  },
};
export interface GraphAttrs {
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

export class Graph implements m.ClassComponent<GraphAttrs> {
  private dragNode?: QueryNode;
  private nodeLayouts: Map<QueryNode, NodeBoxLayout> = new Map();
  private nodeGraphWidth: number = 0;

  oncreate({dom}: m.VnodeDOM<GraphAttrs>) {
    const box = dom as HTMLElement;
    this.nodeGraphWidth = box.getBoundingClientRect().width;

    box.ondragover = (event) => {
      event.preventDefault(); // Allow dropping
    };

    box.ondrop = (event) => {
      this.onDrop(event, box);
    };

    box.ondragend = () => {
      if (this.dragNode) {
        this.dragNode = undefined;
        m.redraw();
      }
    };
  }

  private onDrop = (event: DragEvent, box: HTMLElement) => {
    event.preventDefault();
    if (!this.dragNode) return;
    const dragNodeLayout = this.nodeLayouts.get(this.dragNode);
    if (!dragNodeLayout) return;

    const rect = box.getBoundingClientRect();
    const w = dragNodeLayout.width ?? DEFAULT_NODE_WIDTH;
    const h = dragNodeLayout.height ?? NODE_HEIGHT;

    const x = event.clientX - rect.left - w / 2;
    const y = event.clientY - rect.top - h / 2;

    const initialLayout: NodeBoxLayout = {
      ...dragNodeLayout,
      x: Math.max(0, Math.min(x, rect.width - w)),
      y: Math.max(0, Math.min(y, rect.height - h)),
    };

    const buttonsReservedArea: NodeBoxLayout = {
      x: this.nodeGraphWidth - BUTTONS_AREA_WIDTH - PADDING,
      y: PADDING,
      width: BUTTONS_AREA_WIDTH,
      height: BUTTONS_AREA_HEIGHT,
    };

    const otherLayouts = [...this.nodeLayouts.entries()]
      .filter(([node, _]) => node !== this.dragNode)
      .map(([, layout]) => layout);

    const allLayouts = [...otherLayouts, buttonsReservedArea];

    const newLayout = findNonOverlappingLayout(
      initialLayout,
      allLayouts,
      w,
      h,
      rect,
    );

    this.nodeLayouts.set(this.dragNode, newLayout);
    m.redraw();
  };

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

  private renderEmptyNodeGraph(attrs: GraphAttrs) {
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
          hotkey: 'T',
          onclick: attrs.onAddStdlibTableSource,
        }),
        m(SourceCard, {
          title: 'Slices',
          description: 'Explore all the slices from your trace.',
          icon: 'bar_chart',
          hotkey: 'S',
          onclick: attrs.onAddSlicesSource,
        }),
        m(SourceCard, {
          title: 'Query Node',
          description:
            'Start with a custom SQL query to act as a source for ' +
            'further exploration.',
          icon: 'code',
          hotkey: 'Q',
          onclick: attrs.onAddSqlSource,
        }),
      ),
    );
  }

  private renderControls(attrs: GraphAttrs) {
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

  view({attrs}: m.CVnode<GraphAttrs>) {
    const {rootNodes, onNodeSelected, selectedNode} = attrs;

    const allNodes: QueryNode[] = [];
    for (const root of rootNodes) {
      let curr: QueryNode | undefined = root;
      while (curr) {
        allNodes.push(curr);
        curr = curr.nextNode;
      }
    }

    // Prune layouts for nodes that no longer exist.
    const newLayouts = new Map<QueryNode, NodeBoxLayout>();
    for (const node of allNodes) {
      const layout = this.nodeLayouts.get(node);
      if (layout) {
        newLayouts.set(node, layout);
      }
    }
    this.nodeLayouts = newLayouts;

    const children: m.Child[] = [];

    if (allNodes.length === 0) {
      children.push(this.renderEmptyNodeGraph(attrs));
    } else {
      for (const node of allNodes) {
        let layout = this.nodeLayouts.get(node);
        if (!layout) {
          layout = findNextAvailablePosition(
            node,
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

function findNonOverlappingLayout(
  initialLayout: NodeBoxLayout,
  otherLayouts: NodeBoxLayout[],
  w: number,
  h: number,
  rect: DOMRect,
): NodeBoxLayout {
  const newLayout = {...initialLayout};

  for (const layout of otherLayouts) {
    if (isOverlapping(newLayout, layout, PADDING)) {
      const layoutW = layout.width ?? DEFAULT_NODE_WIDTH;
      const layoutH = layout.height ?? NODE_HEIGHT;

      const right = layout.x + layoutW + PADDING;
      const left = layout.x - w - PADDING;
      const bottom = layout.y + layoutH + PADDING;
      const top = layout.y - h - PADDING;

      const distRight = Math.abs(newLayout.x - right);
      const distLeft = Math.abs(newLayout.x - left);
      const distBottom = Math.abs(newLayout.y - bottom);
      const distTop = Math.abs(newLayout.y - top);

      const minDist = Math.min(distRight, distLeft, distBottom, distTop);

      if (minDist === distRight) {
        newLayout.x = right;
      } else if (minDist === distLeft) {
        newLayout.x = left;
      } else if (minDist === distBottom) {
        newLayout.y = bottom;
      } else {
        newLayout.y = top;
      }
    }
  }

  newLayout.x = Math.max(0, Math.min(newLayout.x, rect.width - w));
  newLayout.y = Math.max(0, Math.min(newLayout.y, rect.height - h));

  return newLayout;
}

function isOverlapping(
  layout1: NodeBoxLayout,
  layout2: NodeBoxLayout,
  padding: number,
): boolean {
  const w1 = layout1.width ?? DEFAULT_NODE_WIDTH;
  const h1 = layout1.height ?? NODE_HEIGHT;
  const w2 = layout2.width ?? DEFAULT_NODE_WIDTH;
  const h2 = layout2.height ?? NODE_HEIGHT;

  return (
    layout1.x < layout2.x + w2 + padding &&
    layout1.x + w1 + padding > layout2.x &&
    layout1.y < layout2.y + h2 + padding &&
    layout1.y + h1 + padding > layout2.y
  );
}

function findNextAvailablePosition(
  node: QueryNode,
  layouts: NodeBoxLayout[],
  nodeGraphWidth: number,
): NodeBoxLayout {
  const w = Math.max(DEFAULT_NODE_WIDTH, node.getTitle().length * 8 + 60);
  const h = NODE_HEIGHT;

  const buttonsReservedArea: NodeBoxLayout = {
    x: nodeGraphWidth - BUTTONS_AREA_WIDTH - PADDING,
    y: PADDING,
    width: BUTTONS_AREA_WIDTH,
    height: BUTTONS_AREA_HEIGHT,
  };

  const allLayouts = [...layouts, buttonsReservedArea];

  let x = PADDING;
  let y = PADDING;

  while (true) {
    const candidateLayout = {x, y, width: w, height: h};
    let isInvalid = false;
    for (const layout of allLayouts) {
      if (isOverlapping(candidateLayout, layout, PADDING)) {
        isInvalid = true;
        x = layout.x + (layout.width ?? w) + PADDING;
        if (x + w > nodeGraphWidth) {
          x = PADDING;
          y = y + h + PADDING;
        }
        break;
      }
    }
    if (!isInvalid) {
      return candidateLayout;
    }
  }
}
