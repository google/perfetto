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

import {Icons} from '../../../../base/semantic_icons';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Intent} from '../../../../widgets/common';
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {QueryNode, singleNodeOperation} from '../../query_node';
import {UIFilter} from '../operations/filter';

import {
  SingleNode,
  NODE_HEIGHT,
  PADDING,
  DEFAULT_NODE_WIDTH,
} from './single_node';
import {NodeBlock} from './node_block';
import {Arrow, Port} from './arrow';
import {EmptyGraph} from '../empty_graph';
import {nodeRegistry} from '../node_registry';

import {isMultiSourceNode, isOverlapping, findBlockOverlap} from '../utils';
import {NodeContainerLayout} from './node_container';

const BUTTONS_AREA_WIDTH = 300;
const BUTTONS_AREA_HEIGHT = 50;

function getTopPort(layout: NodeContainerLayout): Port {
  return {
    x: layout.x + (layout.width ?? DEFAULT_NODE_WIDTH) / 2,
    y: layout.y,
  };
}

function getBottomPort(layout: NodeContainerLayout): Port {
  return {
    x: layout.x + (layout.width ?? DEFAULT_NODE_WIDTH) / 2,
    y: layout.y + (layout.height ?? NODE_HEIGHT),
  };
}

export function getAllNodes(rootNodes: QueryNode[]): QueryNode[] {
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

export interface GraphAttrs {
  readonly rootNodes: QueryNode[];
  readonly selectedNode?: QueryNode;
  readonly nodeLayouts: Map<string, NodeContainerLayout>;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly onDeselect: () => void;
  readonly onNodeLayoutChange: (
    nodeId: string,
    layout: NodeContainerLayout,
  ) => void;
  readonly onAddSourceNode: (id: string) => void;
  readonly onAddOperationNode: (id: string, node: QueryNode) => void;
  readonly onClearAllNodes: () => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
  readonly onImport: () => void;
  readonly onImportWithStatement: () => void;
  readonly onExport: () => void;
  readonly onRemoveFilter: (node: QueryNode, filter: UIFilter) => void;
  readonly devMode?: boolean;
  readonly onDevModeChange?: (enabled: boolean) => void;
}

export class Graph implements m.ClassComponent<GraphAttrs> {
  private attrs?: GraphAttrs;

  // The node currently being dragged. This is used to apply styles and
  // transformations to the node while it is being moved.
  private dragNode?: QueryNode;
  // A map from nodes to their layout information (position and size). This
  // allows us to quickly look up the position of any node in the graph.
  private currentLayouts: Map<QueryNode, NodeContainerLayout> = new Map();
  // The width of the node graph area. This is used to constrain the nodes
  // within the bounds of the graph.
  private nodeGraphWidth: number = 0;
  // The offset of the mouse cursor from the top-left corner of the dragged
  // node. This is used to prevent the node from jumping to the cursor's
  // position when the drag starts.
  private dragOffset?: {x: number; y: number};
  private dragNodeOriginalLayout?: NodeContainerLayout;
  private dragBlockOffsets?: Map<QueryNode, {x: number; y: number}>;
  private revertDrag: boolean = false;

  oncreate({dom, attrs}: m.VnodeDOM<GraphAttrs>) {
    const box = dom as HTMLElement;
    this.nodeGraphWidth = box.getBoundingClientRect().width;

    box.ondragover = (event) => {
      event.preventDefault(); // Allow dropping
      if (this.dragNode) {
        const dragNodeLayout = this.currentLayouts.get(this.dragNode);
        if (dragNodeLayout && this.dragOffset) {
          const rect = box.getBoundingClientRect();
          // To provide real-time feedback to the user, we continuously update
          // the node's position during the drag operation. This allows the
          // connecting arrows to follow the node smoothly.
          const x = event.clientX - rect.left - this.dragOffset.x;
          const y = event.clientY - rect.top - this.dragOffset.y;

          if (this.dragBlockOffsets) {
            for (const [node, offset] of this.dragBlockOffsets.entries()) {
              const layout = this.currentLayouts.get(node);
              if (!layout) continue;
              const nodeW = layout.width ?? DEFAULT_NODE_WIDTH;
              const nodeH = layout.height ?? NODE_HEIGHT;
              this.currentLayouts.set(node, {
                ...layout,
                x: Math.max(0, Math.min(x + offset.x, rect.width - nodeW)),
                y: Math.max(0, Math.min(y + offset.y, rect.height - nodeH)),
              });
            }
          } else {
            const w = dragNodeLayout.width ?? DEFAULT_NODE_WIDTH;
            const h = dragNodeLayout.height ?? NODE_HEIGHT;
            this.currentLayouts.set(this.dragNode, {
              ...dragNodeLayout,
              x: Math.max(0, Math.min(x, rect.width - w)),
              y: Math.max(0, Math.min(y, rect.height - h)),
            });
          }
          m.redraw();
        }
      }
    };

    box.ondrop = (event) => {
      this.onDrop(event, box);
    };

    box.ondragend = () => {
      if (this.dragNode && this.revertDrag) {
        attrs.onNodeLayoutChange(
          this.dragNode.nodeId,
          this.dragNodeOriginalLayout!,
        );
        this.revertDrag = false;
      }
      if (this.dragNode) {
        this.dragNode = undefined;
        this.dragOffset = undefined;
        this.dragNodeOriginalLayout = undefined;
        this.dragBlockOffsets = undefined;
        m.redraw();
      }
    };
  }

  private onDrop = (event: DragEvent, box: HTMLElement) => {
    event.preventDefault();
    if (!this.dragNode || !this.attrs) return;
    const attrs = this.attrs;
    const dragNodeLayout = this.currentLayouts.get(this.dragNode);
    if (!dragNodeLayout) return;

    const {nodeToBlock} = this.identifyNodeBlocks(getAllNodes(attrs.rootNodes));
    const draggedBlock = nodeToBlock.get(this.dragNode);
    if (!draggedBlock) {
      throw new Error('Every node should belong to a block');
    }

    const overlappingNode = findBlockOverlap(draggedBlock, this.currentLayouts);

    if (overlappingNode && isMultiSourceNode(overlappingNode)) {
      const lastNodeInBlock = draggedBlock[draggedBlock.length - 1];

      if (!overlappingNode.prevNodes.includes(lastNodeInBlock)) {
        overlappingNode.prevNodes.push(lastNodeInBlock);
        lastNodeInBlock.nextNodes.push(overlappingNode);
        overlappingNode.onPrevNodesUpdated?.();
      }
      this.revertDrag = true;
      m.redraw();
      return;
    }

    const rect = box.getBoundingClientRect();
    const w = dragNodeLayout.width ?? DEFAULT_NODE_WIDTH;
    const h = dragNodeLayout.height ?? NODE_HEIGHT;

    const buttonsReservedArea: NodeContainerLayout = {
      x: this.nodeGraphWidth - BUTTONS_AREA_WIDTH - PADDING,
      y: PADDING,
      width: BUTTONS_AREA_WIDTH,
      height: BUTTONS_AREA_HEIGHT,
    };

    const otherLayouts = [...this.currentLayouts.entries()]
      .filter(([node, _]) => {
        if (this.dragBlockOffsets) {
          return !this.dragBlockOffsets.has(node);
        }
        return node !== this.dragNode;
      })
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

    if (this.dragBlockOffsets) {
      for (const [node, offset] of this.dragBlockOffsets.entries()) {
        const layout = this.currentLayouts.get(node);
        if (!layout) continue;
        const nodeW = layout.width ?? DEFAULT_NODE_WIDTH;
        const nodeH = layout.height ?? NODE_HEIGHT;
        attrs.onNodeLayoutChange(node.nodeId, {
          x: newLayout.x + offset.x,
          y: newLayout.y + offset.y,
          width: nodeW,
          height: nodeH,
        });
      }
    } else {
      attrs.onNodeLayoutChange(this.dragNode.nodeId, {
        ...newLayout,
        width: w,
        height: h,
      });
    }
    m.redraw();
  };

  onNodeDragStart = (
    node: QueryNode,
    event: DragEvent,
    layout: NodeContainerLayout,
  ) => {
    if (!this.attrs) return;

    const allNodes = getAllNodes(this.attrs.rootNodes);
    this.currentLayouts = new Map<QueryNode, NodeContainerLayout>();
    for (const node of allNodes) {
      const layout = this.attrs.nodeLayouts.get(node.nodeId);
      if (layout) {
        this.currentLayouts.set(node, layout);
      }
    }

    this.dragNode = node;
    this.dragNodeOriginalLayout = {...layout};
    const nodeElem = event.currentTarget as HTMLElement;

    this.currentLayouts.set(node, {
      ...layout,
      width: nodeElem.offsetWidth,
      height: nodeElem.offsetHeight,
    });

    this.dragBlockOffsets = undefined;
    const {nodeToBlock} = this.identifyNodeBlocks(allNodes);
    const block = nodeToBlock.get(node);
    if (block && block.length > 1) {
      const dragNodeLayout = this.currentLayouts.get(node);
      if (dragNodeLayout) {
        this.dragBlockOffsets = new Map();
        for (const blockNode of block) {
          const blockNodeLayout = this.currentLayouts.get(blockNode);
          if (blockNodeLayout) {
            this.dragBlockOffsets.set(blockNode, {
              x: blockNodeLayout.x - dragNodeLayout.x,
              y: blockNodeLayout.y - dragNodeLayout.y,
            });
          }
        }
      }
    }

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

  private renderEmptyNodeGraph(attrs: GraphAttrs) {
    return m(EmptyGraph, {
      onAddSourceNode: attrs.onAddSourceNode,
      onImport: attrs.onImport,
      onImportWithStatement: attrs.onImportWithStatement,
      devMode: attrs.devMode,
      onDevModeChange: attrs.onDevModeChange,
    });
  }

  private renderControls(attrs: GraphAttrs) {
    const sourceMenuItems = nodeRegistry
      .list()
      .filter(([_id, descriptor]) => descriptor.type === 'source')
      .map(([id, descriptor]) => {
        if (descriptor.devOnly && !attrs.devMode) {
          return null;
        }
        return m(MenuItem, {
          label: descriptor.name,
          onclick: () => attrs.onAddSourceNode(id),
        });
      });

    const operationMenuItems = nodeRegistry
      .list()
      .filter(([_id, descriptor]) => descriptor.type === 'multisource')
      .map(([id, descriptor]) => {
        if (descriptor.devOnly && !attrs.devMode) {
          return null;
        }
        return m(MenuItem, {
          label: descriptor.name,
          onclick: () => attrs.onAddSourceNode(id),
        });
      });

    const moreMenuItems = [
      m(MenuItem, {
        label: 'Export',
        icon: Icons.Download,
        onclick: attrs.onExport,
      }),
      m(MenuItem, {
        label: 'Clear All Nodes',
        icon: Icons.Delete,
        intent: Intent.Danger,
        onclick: attrs.onClearAllNodes,
      }),
    ];

    return m(
      '.pf-exp-node-graph__controls',
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            label: 'Add Source',
            icon: Icons.Add,
            variant: ButtonVariant.Filled,
          }),
        },
        sourceMenuItems,
      ),
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            label: 'Add Operation',
            icon: Icons.Add,
            variant: ButtonVariant.Filled,
            style: {marginLeft: '8px'},
          }),
        },
        operationMenuItems,
      ),
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            icon: Icons.ContextMenuAlt,
            variant: ButtonVariant.Minimal,
            style: {marginLeft: '8px'},
          }),
        },
        moreMenuItems,
      ),
    );
  }

  public identifyNodeBlocks(allNodes: QueryNode[]): {
    nodeToBlock: Map<QueryNode, QueryNode[]>;
    renderedAsPartOfBlock: Set<QueryNode>;
  } {
    const renderedAsPartOfBlock = new Set<QueryNode>();
    const nodeToBlock = new Map<QueryNode, QueryNode[]>();

    for (const node of allNodes) {
      if (renderedAsPartOfBlock.has(node)) continue;
      const block: QueryNode[] = [node];
      let currentNode = node;
      while (
        currentNode.nextNodes.length === 1 &&
        singleNodeOperation(currentNode.nextNodes[0].type)
      ) {
        currentNode = currentNode.nextNodes[0];
        block.push(currentNode);
      }
      nodeToBlock.set(node, block);
      for (const blockNode of block) {
        renderedAsPartOfBlock.add(blockNode);
      }
    }
    return {nodeToBlock, renderedAsPartOfBlock};
  }

  private renderNodesAndBlocks(
    attrs: GraphAttrs,
    allNodes: QueryNode[],
    nodeToBlock: Map<QueryNode, QueryNode[]>,
    renderedAsPartOfBlock: Set<QueryNode>,
    onNodeRendered: (node: QueryNode, element: HTMLElement) => void,
  ): m.Child[] {
    const {selectedNode, onNodeSelected} = attrs;
    const children: m.Child[] = [];
    for (const node of allNodes) {
      if (renderedAsPartOfBlock.has(node) && !nodeToBlock.has(node)) {
        continue;
      }

      const block = nodeToBlock.get(node);
      if (block) {
        const layout = this.currentLayouts.get(node)!;
        children.push(
          m(NodeBlock, {
            nodes: block,
            selectedNode,
            layout,
            onNodeSelected,
            onNodeDragStart: this.onNodeDragStart,
            onDuplicateNode: attrs.onDuplicateNode,
            onDeleteNode: attrs.onDeleteNode,
            onAddOperationNode: attrs.onAddOperationNode,
            onNodeRendered,
            onRemoveFilter: attrs.onRemoveFilter,
          }),
        );
      } else {
        const layout = this.currentLayouts.get(node)!;
        children.push(
          m(SingleNode, {
            node,
            isSelected: selectedNode === node,
            layout,
            onNodeSelected,
            onNodeDragStart: this.onNodeDragStart,
            onDuplicateNode: attrs.onDuplicateNode,
            onDeleteNode: attrs.onDeleteNode,
            onAddOperationNode: attrs.onAddOperationNode,
            onNodeRendered,
            onRemoveFilter: attrs.onRemoveFilter,
          }),
        );
      }
    }
    return children;
  }

  private renderArrows(
    allNodes: QueryNode[],
    nodeToBlock: Map<QueryNode, QueryNode[]>,
    renderedAsPartOfBlock: Set<QueryNode>,
  ): m.Child[] {
    const children: m.Child[] = [];
    for (const node of allNodes) {
      if (renderedAsPartOfBlock.has(node) && !nodeToBlock.has(node)) {
        continue;
      }
      const block = nodeToBlock.get(node);
      const lastNodeInGroup = block ? block[block.length - 1] : node;

      for (const nextNode of lastNodeInGroup.nextNodes) {
        const from = this.currentLayouts.get(node);
        const to = this.currentLayouts.get(nextNode);
        if (from && to) {
          const fromPort = getBottomPort(from);
          const toPort = getTopPort(to);
          children.push(m(Arrow, {from: fromPort, to: toPort}));
        }
      }
    }
    return children;
  }

  view({attrs}: m.CVnode<GraphAttrs>) {
    this.attrs = attrs;
    const {rootNodes} = attrs;

    const onNodeRendered = (node: QueryNode, element: HTMLElement) => {
      const layout = this.currentLayouts.get(node);
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

    const allNodes = getAllNodes(rootNodes);

    // Prune layouts for nodes that no longer exist.
    if (!this.dragNode) {
      this.currentLayouts = new Map<QueryNode, NodeContainerLayout>();
      for (const node of allNodes) {
        const layout = attrs.nodeLayouts.get(node.nodeId);
        if (layout) {
          this.currentLayouts.set(node, layout);
        }
      }
    }

    // Pre-flight to calculate layout for new nodes before rendering.
    for (const node of allNodes) {
      if (!this.currentLayouts.has(node)) {
        const newLayout = findNextAvailablePosition(
          node,
          Array.from(this.currentLayouts.values()),
          this.currentLayouts,
          this.nodeGraphWidth,
        );
        this.currentLayouts.set(node, newLayout);
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
      const {nodeToBlock, renderedAsPartOfBlock} =
        this.identifyNodeBlocks(allNodes);

      children.push(
        ...this.renderNodesAndBlocks(
          attrs,
          allNodes,
          nodeToBlock,
          renderedAsPartOfBlock,
          onNodeRendered,
        ),
      );

      children.push(
        ...this.renderArrows(allNodes, nodeToBlock, renderedAsPartOfBlock),
      );

      children.push(this.renderControls(attrs));
    }

    return m(
      '.pf-exp-node-graph',
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
  initialLayout: NodeContainerLayout,
  otherLayouts: NodeContainerLayout[],
  w: number,
  h: number,
  rect: DOMRect,
): NodeContainerLayout {
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

// When a new node is added to the graph, we need to find a suitable position
// for it. This function implements a simple grid-based placement algorithm. It
// iterates through the graph from top to bottom, left to right, and places the
// new node in the first available slot that doesn't overlap with any existing
// nodes.
function findNextAvailablePosition(
  node: QueryNode,
  layouts: NodeContainerLayout[],
  nodeLayouts: Map<QueryNode, NodeContainerLayout>,
  nodeGraphWidth: number,
): NodeContainerLayout {
  const w = Math.max(DEFAULT_NODE_WIDTH, node.getTitle().length * 8 + 60);
  const h = NODE_HEIGHT;

  const buttonsReservedArea: NodeContainerLayout = {
    x: nodeGraphWidth - BUTTONS_AREA_WIDTH - PADDING,
    y: PADDING,
    width: BUTTONS_AREA_WIDTH,
    height: BUTTONS_AREA_HEIGHT,
  };

  const allLayouts = [...layouts, buttonsReservedArea];

  let predecessor: QueryNode | undefined;
  if ('prevNode' in node) {
    predecessor = node.prevNode;
  } else if ('prevNodes' in node && node.prevNodes.length > 0) {
    predecessor = node.prevNodes[0];
  }

  // If the node is a nextNode (e.g., an aggregation or sub-query), it should
  // be added below the previous node.
  if (predecessor) {
    const prevLayout = nodeLayouts.get(predecessor);
    if (prevLayout) {
      let x = prevLayout.x;
      let y = prevLayout.y + (prevLayout.height ?? h) + PADDING * 2;
      // Try to place the new node below the previous node, shifted by the
      // number of siblings.
      if (predecessor.nextNodes.length > 1) {
        x +=
          (predecessor.nextNodes.indexOf(node) -
            (predecessor.nextNodes.length - 1) / 2) *
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
