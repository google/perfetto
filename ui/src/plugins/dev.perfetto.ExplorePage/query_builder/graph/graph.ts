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
import {
  Connection,
  Node,
  NodeGraph,
  NodeGraphApi,
} from '../../../../widgets/nodegraph';
import {UIFilter} from '../operations/filter';
import {
  QueryNode,
  singleNodeOperation,
  SourceNode,
  MultiSourceNode,
  NodeType,
  addConnection,
  removeConnection,
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

// Multi-input nodes (have prevNodes array, cannot be docked)
function isMultiSourceNode(node: QueryNode): node is MultiSourceNode {
  return (
    node.type === NodeType.kIntervalIntersect ||
    node.type === NodeType.kUnion ||
    node.type === NodeType.kMerge
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
  readonly onConnectionRemove: (fromNode: QueryNode, toNode: QueryNode) => void;
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

    // Check if node has custom input labels
    if (
      'getInputLabels' in multiSourceNode &&
      typeof multiSourceNode.getInputLabels === 'function'
    ) {
      return (
        multiSourceNode as MultiSourceNode & {getInputLabels: () => string[]}
      ).getInputLabels();
    }

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

function ensureNodeLayouts(
  roots: QueryNode[],
  attrs: GraphAttrs,
  nodeGraphApi: NodeGraphApi | null,
): void {
  let hasNewNodes = false;
  // Start counting from existing nodes so new nodes don't overlap
  let nodeIndex = attrs.nodeLayouts.size;

  // Give new nodes temporary staggered positions - NodeGraph autoLayout will organize them
  for (const qnode of roots) {
    if (!attrs.nodeLayouts.has(qnode.nodeId)) {
      // Stagger nodes so they don't stack on top of each other
      attrs.onNodeLayoutChange(qnode.nodeId, {
        x: LAYOUT_CONSTANTS.INITIAL_OFFSET + nodeIndex * 50,
        y: LAYOUT_CONSTANTS.INITIAL_OFFSET + nodeIndex * 50,
      });
      hasNewNodes = true;
      nodeIndex++;
    }
  }

  // Let NodeGraph's autoLayout organize all nodes based on connections
  if (hasNewNodes && nodeGraphApi) {
    // Defer autoLayout to next tick so nodes are in DOM
    setTimeout(() => nodeGraphApi.autoLayout(), 0);
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
function renderNodes(
  rootNodes: QueryNode[],
  attrs: GraphAttrs,
  nodeGraphApi: NodeGraphApi | null,
): Node[] {
  const allNodes = getAllNodes(rootNodes);
  const roots = getRootNodes(allNodes, attrs.nodeLayouts);

  ensureNodeLayouts(roots, attrs, nodeGraphApi);

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

  // Convert from 1-indexed port to 0-indexed array for multi-source nodes
  const portIndex = conn.toPort > 0 ? conn.toPort - 1 : undefined;
  addConnection(fromNode, toNode, portIndex);

  m.redraw();
}

// Handles removing a connection (cleans up both forward and backward links)
function handleConnectionRemove(
  conn: Connection,
  rootNodes: QueryNode[],
  onConnectionRemove: (fromNode: QueryNode, toNode: QueryNode) => void,
): void {
  const fromNode = findQueryNode(conn.fromNode, rootNodes);
  const toNode = findQueryNode(conn.toNode, rootNodes);

  if (!fromNode || !toNode) {
    return;
  }

  // Use the helper function to cleanly remove the connection
  removeConnection(fromNode, toNode);

  // Call the parent callback for any additional cleanup (e.g., state management)
  onConnectionRemove(fromNode, toNode);
}

// ========================================
// GRAPH COMPONENT
// ========================================

export class Graph implements m.ClassComponent<GraphAttrs> {
  private nodeGraphApi: NodeGraphApi | null = null;

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

    const nodes = renderNodes(rootNodes, attrs, this.nodeGraphApi);
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
          onReady: (api: NodeGraphApi) => {
            this.nodeGraphApi = api;
          },
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
            handleConnectionRemove(
              connections[index],
              rootNodes,
              attrs.onConnectionRemove,
            );
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
