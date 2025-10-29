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

/**
 * Graph Component - Query Builder Visual Graph Editor
 *
 * This file implements the visual graph editor for the Explore Page query builder.
 * It handles the rendering, layout, and interaction of query nodes in a node-graph format.
 *
 * Key Concepts:
 * - Root Nodes: Nodes that have explicit x,y coordinates and are rendered at top level
 * - Docked Nodes: Child nodes without coordinates that appear "docked" below their parent
 * - Node Chain: A sequence of single-input/single-output nodes connected vertically
 * - Layout Map: Tracks which nodes have explicit coordinates (undocked nodes)
 *
 * Architecture:
 * - Nodes without layout coordinates are rendered "docked" to their parent via the 'next' property
 * - Only root (undocked) nodes need explicit positioning
 * - Connections between undocked nodes are rendered as edges
 * - Docked chains appear as stacked boxes within a single visual unit
 */

import m from 'mithril';

import {Icons} from '../../../../base/semantic_icons';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Intent} from '../../../../widgets/common';
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {Connection, Node, NodeGraph} from '../../../../widgets/nodegraph';
import {UIFilter} from '../operations/filter';
import {
  QueryNode,
  singleNodeOperation,
  SourceNode,
  ModificationNode,
  MultiSourceNode,
  NodeType,
} from '../../query_node';
import {EmptyGraph} from '../empty_graph';
import {nodeRegistry} from '../node_registry';
import {NodeBox} from './node_box';

// ========================================
// TYPE DEFINITIONS
// ========================================

type Position = {x: number; y: number};

// Maps node IDs to their layout positions.
// Nodes in this map are "undocked" (have coordinates), nodes absent are "docked" (attached to parent).
type LayoutMap = Map<string, Position>;

const LAYOUT_CONSTANTS = {
  VERTICAL_SPACING: 150,
  HORIZONTAL_SPACING: 250,
  ROW_SPACING: 200,
  GRID_COLUMNS: 4,
  INITIAL_OFFSET: 100,
};

// ========================================
// TYPE GUARDS
// ========================================

function isSourceNode(node: QueryNode): node is SourceNode {
  return (
    node.type === NodeType.kTable ||
    node.type === NodeType.kSimpleSlices ||
    node.type === NodeType.kSqlSource
  );
}

// Single-input nodes that can be "docked" in a vertical chain
function isModificationNode(node: QueryNode): node is ModificationNode {
  return singleNodeOperation(node.type);
}

// Multi-input nodes (have prevNodes array, cannot be docked)
function isMultiSourceNode(node: QueryNode): node is MultiSourceNode {
  return (
    node.type === NodeType.kIntervalIntersect || node.type === NodeType.kUnion
  );
}

// ========================================
// GRAPH ATTRIBUTES INTERFACE
// ========================================

export interface GraphAttrs {
  readonly rootNodes: QueryNode[];
  readonly selectedNode?: QueryNode;
  readonly nodeLayouts: LayoutMap;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly onDeselect: () => void;
  readonly onNodeLayoutChange: (nodeId: string, layout: Position) => void;
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

// ========================================
// UTILITY FUNCTIONS
// ========================================

// Traverses the graph using BFS with cycle detection (visited set prevents infinite loops)
export function getAllNodes(rootNodes: QueryNode[]): QueryNode[] {
  const allNodes: QueryNode[] = [];
  const visited = new Set<string>();

  for (const root of rootNodes) {
    const queue: QueryNode[] = [root];

    while (queue.length > 0) {
      const curr = queue.shift();
      if (!curr) continue;

      if (visited.has(curr.nodeId)) {
        continue;
      }

      visited.add(curr.nodeId);
      allNodes.push(curr);

      for (const child of curr.nextNodes) {
        if (child !== undefined && !visited.has(child.nodeId)) {
          queue.push(child);
        }
      }
    }
  }
  return allNodes;
}

function findQueryNode(
  nodeId: string,
  rootNodes: QueryNode[],
): QueryNode | undefined {
  const allNodes = getAllNodes(rootNodes);
  return allNodes.find((n) => n.nodeId === nodeId);
}

// A node is "docked" if it has no layout (rendered as part of parent's chain via 'next' property)
function isChildDocked(child: QueryNode, nodeLayouts: LayoutMap): boolean {
  return !nodeLayouts.has(child.nodeId);
}

// ========================================
// NODE PORT AND MENU UTILITIES
// ========================================

function getInputLabels(node: QueryNode): string[] {
  if (isSourceNode(node)) {
    return [];
  }

  if (isMultiSourceNode(node)) {
    const multiSourceNode = node as MultiSourceNode;
    const numConnected = multiSourceNode.prevNodes.filter(
      (it: QueryNode | undefined) => it,
    ).length;
    // Always show one extra empty port for adding new connections
    const numPorts = numConnected + 1;
    const labels: string[] = [];
    for (let i = 0; i < numPorts; i++) {
      labels.push(`Input ${i + 1}`);
    }
    return labels;
  }

  return ['Input'];
}

function buildMenuItems(
  nodeType: 'source' | 'multisource' | 'modification',
  devMode: boolean | undefined,
  onAddNode: (id: string) => void,
): m.Children[] {
  return nodeRegistry
    .list()
    .filter(([_id, descriptor]) => descriptor.type === nodeType)
    .map(([id, descriptor]) => {
      if (descriptor.devOnly && !devMode) {
        return null;
      }
      return m(MenuItem, {
        label: descriptor.name,
        onclick: () => onAddNode(id),
      });
    });
}

function buildAddMenuItems(
  targetNode: QueryNode,
  onAddOperationNode: (id: string, node: QueryNode) => void,
): m.Children[] {
  return buildMenuItems('modification', undefined, (id) =>
    onAddOperationNode(id, targetNode),
  );
}

// ========================================
// LAYOUT UTILITIES
// ========================================

// Returns nodes that should be rendered at the root level (excludes docked children)
function getRootNodes(
  allNodes: QueryNode[],
  nodeLayouts: LayoutMap,
): QueryNode[] {
  const dockedNodes = new Set<QueryNode>();

  // Find all nodes that are docked to their parent
  for (const node of allNodes) {
    if (
      node.nextNodes.length === 1 &&
      node.nextNodes[0] !== undefined &&
      singleNodeOperation(node.nextNodes[0].type) &&
      isChildDocked(node.nextNodes[0], nodeLayouts)
    ) {
      dockedNodes.add(node.nextNodes[0]);
    }
  }

  return allNodes.filter((n) => !dockedNodes.has(n));
}

function findParentLayout(
  qnode: QueryNode,
  nodeLayouts: LayoutMap,
): Position | undefined {
  if (isModificationNode(qnode) && qnode.prevNode) {
    return nodeLayouts.get(qnode.prevNode.nodeId);
  }

  if (isMultiSourceNode(qnode) && qnode.prevNodes.length > 0) {
    const firstParent = qnode.prevNodes.find((n) => n);
    if (firstParent) {
      return nodeLayouts.get(firstParent.nodeId);
    }
  }

  return undefined;
}

// Places nodes in a grid layout (left-to-right, top-to-bottom)
function calculateGridPosition(
  roots: QueryNode[],
  nodeLayouts: LayoutMap,
): Position {
  const existingRootPositions = roots
    .map((r) => nodeLayouts.get(r.nodeId))
    .filter((layout): layout is Position => layout !== undefined);

  const count = existingRootPositions.length;
  const col = count % LAYOUT_CONSTANTS.GRID_COLUMNS;
  const row = Math.floor(count / LAYOUT_CONSTANTS.GRID_COLUMNS);

  return {
    x:
      LAYOUT_CONSTANTS.INITIAL_OFFSET +
      col * LAYOUT_CONSTANTS.HORIZONTAL_SPACING,
    y: LAYOUT_CONSTANTS.INITIAL_OFFSET + row * LAYOUT_CONSTANTS.ROW_SPACING,
  };
}

// Calculates position for a new node: below parent if it exists, otherwise in grid
function calculateDefaultLayout(
  qnode: QueryNode,
  roots: QueryNode[],
  nodeLayouts: LayoutMap,
): Position {
  const parentLayout = findParentLayout(qnode, nodeLayouts);

  if (parentLayout) {
    return {
      x: parentLayout.x,
      y: parentLayout.y + LAYOUT_CONSTANTS.VERTICAL_SPACING,
    };
  }

  return calculateGridPosition(roots, nodeLayouts);
}

function ensureNodeLayouts(roots: QueryNode[], attrs: GraphAttrs): void {
  for (const qnode of roots) {
    if (!attrs.nodeLayouts.has(qnode.nodeId)) {
      const defaultLayout = calculateDefaultLayout(
        qnode,
        roots,
        attrs.nodeLayouts,
      );
      attrs.onNodeLayoutChange(qnode.nodeId, defaultLayout);
    }
  }
}

// ========================================
// NODE RENDERING
// ========================================

// Assigns a color hue based on the node's type for visual distinction
function getNodeHue(node: QueryNode): number {
  switch (node.type) {
    case NodeType.kTable:
      return 354; // Red (#ffcdd2)
    case NodeType.kSimpleSlices:
      return 122; // Green (#c8e6c9)
    case NodeType.kSqlSource:
      return 199; // Cyan/Light Blue (#b3e5fc)
    case NodeType.kAggregation:
      return 339; // Pink (#f8bbd0)
    case NodeType.kModifyColumns:
      return 261; // Purple (#d1c4e9)
    case NodeType.kAddColumns:
      return 232; // Indigo (#c5cae9)
    case NodeType.kLimitAndOffset:
      return 175; // Teal (#b2dfdb)
    case NodeType.kSort:
      return 54; // Yellow (#fff9c4)
    case NodeType.kIntervalIntersect:
      return 45; // Amber/Orange (#ffecb3)
    case NodeType.kUnion:
      return 187; // Cyan (#b2ebf2)
    default:
      return 65; // Lime (#f0f4c3)
  }
}

// Returns the next docked child in the chain (rendered via 'next' property)
function getNextDockedNode(
  qnode: QueryNode,
  attrs: GraphAttrs,
): Omit<Node, 'x' | 'y'> | undefined {
  if (
    qnode.nextNodes.length === 1 &&
    qnode.nextNodes[0] !== undefined &&
    singleNodeOperation(qnode.nextNodes[0].type) &&
    isChildDocked(qnode.nextNodes[0], attrs.nodeLayouts)
  ) {
    return renderChildNode(qnode.nextNodes[0], attrs);
  }
  return undefined;
}

function createNodeConfig(
  qnode: QueryNode,
  attrs: GraphAttrs,
): Omit<Node, 'x' | 'y'> {
  return {
    id: qnode.nodeId,
    inputs: getInputLabels(qnode),
    outputs: ['Output'],
    hue: getNodeHue(qnode),
    accentBar: true,
    content: m(NodeBox, {
      node: qnode,
      onDuplicateNode: attrs.onDuplicateNode,
      onDeleteNode: attrs.onDeleteNode,
      onAddOperationNode: attrs.onAddOperationNode,
      onRemoveFilter: attrs.onRemoveFilter,
    }),
    next: getNextDockedNode(qnode, attrs),
    addMenuItems: buildAddMenuItems(qnode, attrs.onAddOperationNode),
    allInputsLeft: isMultiSourceNode(qnode),
  };
}

function renderChildNode(
  qnode: QueryNode,
  attrs: GraphAttrs,
): Omit<Node, 'x' | 'y'> {
  return createNodeConfig(qnode, attrs);
}

function renderNodeChain(
  qnode: QueryNode,
  layout: Position,
  attrs: GraphAttrs,
): Node {
  return {
    ...createNodeConfig(qnode, attrs),
    x: layout.x,
    y: layout.y,
  };
}

// Renders only root nodes; docked children are recursively rendered via 'next' property
function renderNodes(rootNodes: QueryNode[], attrs: GraphAttrs): Node[] {
  const allNodes = getAllNodes(rootNodes);
  const roots = getRootNodes(allNodes, attrs.nodeLayouts);

  ensureNodeLayouts(roots, attrs);

  return roots
    .map((qnode) => {
      const layout = attrs.nodeLayouts.get(qnode.nodeId);
      if (!layout) {
        console.warn(`Node ${qnode.nodeId} has no layout, skipping render.`);
        return null;
      }
      return renderNodeChain(qnode, layout, attrs);
    })
    .filter((n): n is Node => n !== null);
}

// ========================================
// CONNECTION HANDLING
// ========================================

// For multi-source nodes, finds which input port (1-indexed) the parent is connected to
function calculateInputPort(child: QueryNode, parent: QueryNode): number {
  if (!isMultiSourceNode(child)) {
    return 0;
  }

  const index = child.prevNodes.indexOf(parent);
  return index !== -1 ? index + 1 : 0;
}

// Builds visual connections between nodes (skips docked chains since they use 'next' property)
function buildConnections(
  rootNodes: QueryNode[],
  nodeLayouts: LayoutMap,
): Connection[] {
  const connections: Connection[] = [];
  const allNodes = getAllNodes(rootNodes);

  for (const qnode of allNodes) {
    for (const child of qnode.nextNodes) {
      if (child === undefined) continue;

      // Skip docked children - they're rendered via 'next' property, not as connections
      if (
        qnode.nextNodes.length === 1 &&
        singleNodeOperation(child.type) &&
        isChildDocked(child, nodeLayouts)
      ) {
        continue;
      }

      connections.push({
        fromNode: qnode.nodeId,
        fromPort: 0,
        toNode: child.nodeId,
        toPort: calculateInputPort(child, qnode),
      });
    }
  }

  return connections;
}

// Handles creating a new connection between nodes (updates both forward and backward links)
function handleConnect(conn: Connection, rootNodes: QueryNode[]): void {
  const fromNode = findQueryNode(conn.fromNode, rootNodes);
  const toNode = findQueryNode(conn.toNode, rootNodes);

  if (!fromNode || !toNode) {
    return;
  }

  // Update forward link
  if (!fromNode.nextNodes.includes(toNode)) {
    fromNode.nextNodes.push(toNode);
  }

  // Update backward link based on node type
  if (isModificationNode(toNode)) {
    toNode.prevNode = fromNode;
  } else if (isMultiSourceNode(toNode)) {
    const arrayIndex = conn.toPort - 1; // Convert from 1-indexed to 0-indexed

    // Expand array if needed to accommodate the new connection
    while (toNode.prevNodes.length <= arrayIndex) {
      toNode.prevNodes.push(undefined);
    }

    toNode.prevNodes[arrayIndex] = fromNode;
    toNode.onPrevNodesUpdated?.();
  }

  m.redraw();
}

// Handles removing a connection (cleans up both forward and backward links)
function handleConnectionRemove(
  conn: Connection,
  rootNodes: QueryNode[],
): void {
  const fromNode = findQueryNode(conn.fromNode, rootNodes);
  const toNode = findQueryNode(conn.toNode, rootNodes);

  if (!fromNode || !toNode) {
    return;
  }

  // Remove forward link
  const idx = fromNode.nextNodes.indexOf(toNode);
  if (idx !== -1) {
    fromNode.nextNodes.splice(idx, 1);
  }

  // Clear backward link
  if (isModificationNode(toNode) && toNode.prevNode === fromNode) {
    toNode.prevNode = undefined;
  } else if (isMultiSourceNode(toNode)) {
    const prevIndex = toNode.prevNodes.indexOf(fromNode);
    if (prevIndex !== -1) {
      toNode.prevNodes[prevIndex] = undefined;
    }
    toNode.onPrevNodesUpdated?.();
  }

  m.redraw();
}

// ========================================
// GRAPH COMPONENT
// ========================================

export class Graph implements m.ClassComponent<GraphAttrs> {
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
    const sourceMenuItems = buildMenuItems(
      'source',
      attrs.devMode,
      attrs.onAddSourceNode,
    );

    const operationMenuItems = buildMenuItems(
      'multisource',
      attrs.devMode,
      attrs.onAddSourceNode,
    );

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

  view({attrs}: m.CVnode<GraphAttrs>) {
    const {rootNodes, selectedNode} = attrs;
    const allNodes = getAllNodes(rootNodes);

    if (allNodes.length === 0) {
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
        this.renderEmptyNodeGraph(attrs),
      );
    }

    const nodes = renderNodes(rootNodes, attrs);
    const connections = buildConnections(rootNodes, attrs.nodeLayouts);

    return m(
      '.pf-exp-node-graph',
      {
        tabindex: 0,
      },
      [
        this.renderControls(attrs),
        m(NodeGraph, {
          nodes,
          connections,
          selectedNodeId: selectedNode?.nodeId ?? null,
          hideControls: true,
          onNodeSelect: (nodeId: string | null) => {
            if (nodeId === null) {
              attrs.onDeselect();
            } else {
              const qnode = findQueryNode(nodeId, rootNodes);
              if (qnode) {
                attrs.onNodeSelected(qnode);
              }
            }
          },
          onNodeDrag: (nodeId: string, x: number, y: number) => {
            attrs.onNodeLayoutChange(nodeId, {x, y});
          },
          onConnect: (conn: Connection) => {
            handleConnect(conn, rootNodes);
          },
          onConnectionRemove: (index: number) => {
            handleConnectionRemove(connections[index], rootNodes);
          },
          onNodeRemove: (nodeId: string) => {
            const qnode = findQueryNode(nodeId, rootNodes);
            if (qnode) {
              attrs.onDeleteNode(qnode);
            }
          },
          onUndock: () => {
            // When undocking, NodeGraph widget assigns x,y via onNodeDrag callback
            // The node relationships (nextNodes/prevNode) remain unchanged
            m.redraw();
          },
          onDock: (_targetId: string, childNode: Omit<Node, 'x' | 'y'>) => {
            // Remove coordinates so node becomes "docked" (renders via parent's 'next')
            attrs.nodeLayouts.delete(childNode.id);
            m.redraw();
          },
        }),
      ],
    );
  }
}
