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
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {QueryNode} from '../query_node';
import {FilterDefinition} from '../../../components/widgets/data_grid/common';

import {
  NodeBox,
  NodeBoxLayout,
  NODE_HEIGHT,
  PADDING,
  DEFAULT_NODE_WIDTH,
} from './node_box';
import {Arrow, Port} from './arrow';
import {Icon} from '../../../widgets/icon';

const BUTTONS_AREA_WIDTH = 300;
const BUTTONS_AREA_HEIGHT = 50;

function getOutputPorts(layout: NodeBoxLayout, portCount: number): Port[] {
  const ports: Port[] = [];
  for (let i = 0; i < portCount; i++) {
    ports.push({
      x:
        layout.x +
        ((layout.width ?? DEFAULT_NODE_WIDTH) * (i + 1)) / (portCount + 1),
      y: layout.y + (layout.height ?? NODE_HEIGHT),
    });
  }
  return ports;
}

function getInputPorts(layout: NodeBoxLayout, portCount: number): Port[] {
  const ports: Port[] = [];
  for (let i = 0; i < portCount; i++) {
    ports.push({
      x:
        layout.x +
        ((layout.width ?? DEFAULT_NODE_WIDTH) * (i + 1)) / (portCount + 1),
      y: layout.y,
    });
  }
  return ports;
}

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
  readonly nodeLayouts: Map<string, NodeBoxLayout>;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly onDeselect: () => void;
  readonly onNodeLayoutChange: (nodeId: string, layout: NodeBoxLayout) => void;
  readonly onAddStdlibTableSource: () => void;
  readonly onAddSlicesSource: () => void;
  readonly onAddSqlSource: () => void;
  readonly onAddAggregation: (node: QueryNode) => void;
  readonly onAddModifyColumns: (node: QueryNode) => void;
  readonly onAddIntervalIntersect: (node: QueryNode) => void;
  readonly onClearAllNodes: () => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
  readonly onImport: () => void;
  readonly onImportWithStatement: () => void;
  readonly onExport: () => void;
  readonly onRemoveFilter: (node: QueryNode, filter: FilterDefinition) => void;
}

export class Graph implements m.ClassComponent<GraphAttrs> {
  private attrs?: GraphAttrs;

  // The node currently being dragged. This is used to apply styles and
  // transformations to the node while it is being moved.
  private dragNode?: QueryNode;
  // A map from nodes to their layout information (position and size). This
  // allows us to quickly look up the position of any node in the graph.
  private resolvedNodeLayouts: Map<QueryNode, NodeBoxLayout> = new Map();
  // The width of the node graph area. This is used to constrain the nodes
  // within the bounds of the graph.
  private nodeGraphWidth: number = 0;
  // The offset of the mouse cursor from the top-left corner of the dragged
  // node. This is used to prevent the node from jumping to the cursor's
  // position when the drag starts.
  private dragOffset?: {x: number; y: number};

  oncreate({dom, attrs}: m.VnodeDOM<GraphAttrs>) {
    const box = dom as HTMLElement;
    this.nodeGraphWidth = box.getBoundingClientRect().width;

    box.ondragover = (event) => {
      event.preventDefault(); // Allow dropping
      if (this.dragNode) {
        const dragNodeLayout = this.resolvedNodeLayouts.get(this.dragNode);
        if (dragNodeLayout && this.dragOffset) {
          const rect = box.getBoundingClientRect();
          const w = dragNodeLayout.width ?? DEFAULT_NODE_WIDTH;
          const h = dragNodeLayout.height ?? NODE_HEIGHT;
          // To provide real-time feedback to the user, we continuously update
          // the node's position during the drag operation. This allows the
          // connecting arrows to follow the node smoothly.
          const x = event.clientX - rect.left - this.dragOffset.x;
          const y = event.clientY - rect.top - this.dragOffset.y;
          this.resolvedNodeLayouts.set(this.dragNode, {
            ...dragNodeLayout,
            x: Math.max(0, Math.min(x, rect.width - w)),
            y: Math.max(0, Math.min(y, rect.height - h)),
          });
          m.redraw();
        }
      }
    };

    box.ondrop = (event) => {
      this.onDrop(event, box, attrs);
    };

    box.ondragend = () => {
      if (this.dragNode) {
        this.dragNode = undefined;
        this.dragOffset = undefined;
        m.redraw();
      }
    };
  }

  private onDrop = (event: DragEvent, box: HTMLElement, attrs: GraphAttrs) => {
    event.preventDefault();
    if (!this.dragNode) return;
    const dragNodeLayout = this.resolvedNodeLayouts.get(this.dragNode);
    if (!dragNodeLayout) return;

    const rect = box.getBoundingClientRect();
    const w = dragNodeLayout.width ?? DEFAULT_NODE_WIDTH;
    const h = dragNodeLayout.height ?? NODE_HEIGHT;

    // The "Add Node" and "Clear All Nodes" buttons occupy a fixed area in the
    // top-right corner of the graph. To prevent nodes from being dropped on
    // top of these buttons, we define a reserved area that is treated as an
    // obstacle.
    const buttonsReservedArea: NodeBoxLayout = {
      x: this.nodeGraphWidth - BUTTONS_AREA_WIDTH - PADDING,
      y: PADDING,
      width: BUTTONS_AREA_WIDTH,
      height: BUTTONS_AREA_HEIGHT,
    };

    const otherLayouts = [...this.resolvedNodeLayouts.entries()]
      .filter(([node, _]) => node !== this.dragNode)
      .map(([, layout]) => layout);

    const allLayouts = [...otherLayouts, buttonsReservedArea];

    // After the node is dropped, we need to find a final position for it that
    // doesn't overlap with any other nodes. This is important because the
    // user can drag the node over other nodes, and we want to ensure that
    // the graph is still readable after the drag operation is complete.
    const newLayout = findNonOverlappingLayout(
      dragNodeLayout,
      allLayouts,
      w,
      h,
      rect,
    );

    attrs.onNodeLayoutChange(this.dragNode.nodeId, {
      ...newLayout,
      width: w,
      height: h,
    });
    m.redraw();
  };

  onNodeDragStart = (
    node: QueryNode,
    event: DragEvent,
    layout: NodeBoxLayout,
  ) => {
    if (!this.attrs) return;

    const allNodes = this.getAllNodes(this.attrs.rootNodes);
    this.resolvedNodeLayouts = new Map<QueryNode, NodeBoxLayout>();
    for (const node of allNodes) {
      const layout = this.attrs.nodeLayouts.get(node.nodeId);
      if (layout) {
        this.resolvedNodeLayouts.set(node, layout);
      }
    }

    this.dragNode = node;
    const nodeElem = (event.target as HTMLElement).closest(
      '.pf-node-box',
    ) as HTMLElement;

    this.resolvedNodeLayouts.set(node, {
      ...layout,
      width: nodeElem.offsetWidth,
      height: nodeElem.offsetHeight,
    });

    // To prevent the node from jumping to the cursor's position when a drag
    // starts, we calculate the initial offset of the cursor from the
    // top-left corner of the node. This offset is then used to maintain the
    // node's position relative to the cursor throughout the drag operation.
    const rect = nodeElem.getBoundingClientRect();
    this.dragOffset = {
      x: event.clientX - rect.left,
      y: event.clientY - rect.top,
    };

    if (event.dataTransfer) {
      event.dataTransfer.setData('text/plain', node.getTitle());
      event.dataTransfer.effectAllowed = 'move';
    }
  };

  private getAllNodes(rootNodes: QueryNode[]): QueryNode[] {
    const allNodes: QueryNode[] = [];
    for (const root of rootNodes) {
      const queue: QueryNode[] = [root];
      while (queue.length > 0) {
        const curr = queue.shift()!;
        allNodes.push(curr);
        for (const child of curr.nextNodes) {
          queue.push(child);
        }
      }
    }
    return allNodes;
  }

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
      m(Button, {
        label: 'Import',
        onclick: attrs.onImport,
        variant: ButtonVariant.Filled,
        icon: 'file_upload',
      }),
      m(Button, {
        label: 'Import from WITH statement',
        onclick: attrs.onImportWithStatement,
        variant: ButtonVariant.Filled,
        icon: 'code',
        style: {marginLeft: '8px'},
      }),
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
            variant: ButtonVariant.Filled,
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
        label: 'Export',
        icon: Icons.Download,
        variant: ButtonVariant.Minimal,
        onclick: attrs.onExport,
        style: {marginLeft: '8px'},
      }),
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
    this.attrs = attrs;
    const {rootNodes, onNodeSelected, selectedNode} = attrs;

    const onNodeRendered = (node: QueryNode, element: HTMLElement) => {
      const layout = this.resolvedNodeLayouts.get(node);
      if (layout) {
        const newWidth = element.offsetWidth;
        const newHeight = element.offsetHeight;
        if (layout.width !== newWidth || layout.height !== newHeight) {
          attrs.onNodeLayoutChange(node.nodeId, {
            ...layout,
            width: newWidth,
            height: newHeight,
          });
        }
      }
    };

    const allNodes = this.getAllNodes(rootNodes);

    // Prune layouts for nodes that no longer exist.
    if (!this.dragNode) {
      this.resolvedNodeLayouts = new Map<QueryNode, NodeBoxLayout>();
      for (const node of allNodes) {
        const layout = attrs.nodeLayouts.get(node.nodeId);
        if (layout) {
          this.resolvedNodeLayouts.set(node, layout);
        }
      }
    }

    // Pre-flight to calculate layout for new nodes before rendering.
    for (const node of allNodes) {
      if (!this.resolvedNodeLayouts.has(node)) {
        const newLayout = findNextAvailablePosition(
          node,
          Array.from(this.resolvedNodeLayouts.values()),
          this.resolvedNodeLayouts,
          this.nodeGraphWidth,
        );
        this.resolvedNodeLayouts.set(node, newLayout);
        attrs.onNodeLayoutChange(node.nodeId, {
          x: newLayout.x,
          y: newLayout.y,
        });
      }
    }

    const children: m.Child[] = [];

    if (allNodes.length === 0) {
      children.push(this.renderEmptyNodeGraph(attrs));
    } else {
      for (const node of allNodes) {
        const layout = this.resolvedNodeLayouts.get(node)!;
        const inputPorts = getInputPorts(layout, node.prevNodes?.length ?? 1);
        for (let i = 0; i < (node.prevNodes?.length ?? 0); i++) {
          const prevNode = node.prevNodes![i];
          const from = this.resolvedNodeLayouts.get(prevNode);
          if (from) {
            const outputPorts = getOutputPorts(from, prevNode.nextNodes.length);
            const fromPort =
              outputPorts[prevNode.nextNodes.indexOf(node)] ?? outputPorts[0];
            const toPort = inputPorts[i];
            children.push(m(Arrow, {from: fromPort, to: toPort}));
          }
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
            onAddAggregation: attrs.onAddAggregation,
            onModifyColumns: attrs.onAddModifyColumns,
            onAddIntervalIntersect: attrs.onAddIntervalIntersect,
            onNodeRendered,
            onRemoveFilter: attrs.onRemoveFilter,
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

// When a node is dropped, it might overlap with other nodes. This function
// resolves such overlaps by finding the nearest available position for the
// node. It works by checking for collisions and then shifting the node just
// enough to clear the obstacle. This process is repeated until no more
// overlaps are detected.
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

      // To resolve an overlap, we can move the node in one of four
      // directions: right, left, down, or up. We calculate the target
      // position for each of these moves.
      const right = layout.x + layoutW + PADDING;
      const left = layout.x - w - PADDING;
      const bottom = layout.y + layoutH + PADDING;
      const top = layout.y - h - PADDING;

      // We want to move the node by the smallest possible amount to resolve
      // the overlap. To do this, we calculate the distance to each of the
      // four possible positions.
      const distRight = Math.abs(newLayout.x - right);
      const distLeft = Math.abs(newLayout.x - left);
      const distBottom = Math.abs(newLayout.y - bottom);
      const distTop = Math.abs(newLayout.y - top);

      // The shortest distance determines the direction in which the node will
      // be moved.
      const minDist = Math.min(distRight, distLeft, distBottom, distTop);

      // By moving the node to the closest non-overlapping position, we
      // ensure that the layout remains as stable as possible after the drag
      // operation is complete.
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

  // Finally, we ensure that the new layout is still within the bounds of the
  // graph. This prevents nodes from being moved outside of the visible area.
  newLayout.x = Math.max(0, Math.min(newLayout.x, rect.width - w));
  newLayout.y = Math.max(0, Math.min(newLayout.y, rect.height - h));

  return newLayout;
}

// This is a standard axis-aligned bounding box (AABB) collision detection
// algorithm. It checks if two rectangles are overlapping by comparing their
// positions and dimensions.
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

// When a new node is added to the graph, we need to find a suitable position
// for it. This function implements a simple grid-based placement algorithm. It
// iterates through the graph from top to bottom, left to right, and places the
// new node in the first available slot that doesn't overlap with any existing
// nodes.
function findNextAvailablePosition(
  node: QueryNode,
  layouts: NodeBoxLayout[],
  nodeLayouts: Map<QueryNode, NodeBoxLayout>,
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

  // If the node is a nextNode (e.g., an aggregation or sub-query), it should
  // be added below the previous node.
  if (node.prevNodes && node.prevNodes.length > 0) {
    const prevLayout = nodeLayouts.get(node.prevNodes[0]);
    if (prevLayout) {
      let x = prevLayout.x;
      let y = prevLayout.y + (prevLayout.height ?? h) + PADDING * 2;
      // Try to place the new node below the previous node, shifted by the
      // number of siblings.
      if (node.prevNodes[0].nextNodes.length > 1) {
        x +=
          (node.prevNodes[0].nextNodes.indexOf(node) -
            (node.prevNodes[0].nextNodes.length - 1) / 2) *
          (w + PADDING);
      }
      while (true) {
        const candidateLayout = {x, y, width: w, height: h};
        let isInvalid = false;
        for (const layout of allLayouts) {
          if (isOverlapping(candidateLayout, layout, PADDING)) {
            isInvalid = true;
            y = layout.y + (layout.height ?? h) + PADDING;
            break;
          }
        }
        if (!isInvalid) {
          return candidateLayout;
        }
      }
    }
  }

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
