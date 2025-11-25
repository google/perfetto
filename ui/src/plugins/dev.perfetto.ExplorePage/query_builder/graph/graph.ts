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
import {
  MenuItem,
  MenuDivider,
  MenuTitle,
  PopupMenu,
} from '../../../../widgets/menu';
import {
  Connection,
  Node,
  NodeGraph,
  NodeGraphApi,
  NodeGraphAttrs,
  NodePort,
} from '../../../../widgets/nodegraph';
import {
  QueryNode,
  singleNodeOperation,
  SourceNode,
  MultiSourceNode,
  ModificationNode,
  NodeType,
  addConnection,
  removeConnection,
} from '../../query_node';
import {EmptyGraph} from '../empty_graph';
import {nodeRegistry} from '../node_registry';
import {NodeBox} from './node_box';
import {buildCategorizedMenuItems} from './menu_utils';

// ========================================
// TYPE DEFINITIONS
// ========================================

type Position = {x: number; y: number};

// Maps node IDs to their layout positions.
// Nodes in this map are "undocked" (have coordinates), nodes absent are "docked" (attached to parent).
type LayoutMap = Map<string, Position>;

const LAYOUT_CONSTANTS = {
  INITIAL_X: 100,
  INITIAL_Y: 100,
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

function getInputLabels(node: QueryNode): NodePort[] {
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
      )
        .getInputLabels()
        .map((label) => ({content: label, direction: 'left'}));
    }

    // Always show one extra empty port for adding new connections
    const numPorts = multiSourceNode.prevNodes.length + 1;
    const labels: NodePort[] = [];
    for (let i = 0; i < numPorts; i++) {
      labels.push({content: `Input ${i + 1}`, direction: 'left'});
    }
    return labels;
  }

  // Check if ModificationNode has inputNodes (additional left-side inputs)
  if ('inputNodes' in node) {
    const modNode = node as ModificationNode;
    if (modNode.inputNodes !== undefined && Array.isArray(modNode.inputNodes)) {
      // Check if node has custom input labels
      if (
        'getInputLabels' in modNode &&
        typeof modNode.getInputLabels === 'function'
      ) {
        return modNode.getInputLabels();
      }

      const labels: NodePort[] = [];

      // Add top port for prevNode (main data flow)
      labels.push({content: 'Input', direction: 'top'});

      // For AddColumnsNode, show exactly one left-side port
      // (it only supports connecting one table to add columns from)
      if ('type' in modNode && modNode.type === NodeType.kAddColumns) {
        labels.push({content: 'Table', direction: 'left'});
        return labels;
      }

      // For other nodes with inputNodes, dynamically show ports
      const numConnected = modNode.inputNodes.filter(
        (it: QueryNode | undefined) => it,
      ).length;
      // Always show one extra empty port for adding new connections
      const numLeftPorts = numConnected + 1;

      // Add left-side ports for inputNodes (additional table inputs)
      for (let i = 0; i < numLeftPorts; i++) {
        labels.push({content: `Table ${i + 1}`, direction: 'left'});
      }
      return labels;
    }
  }

  return [{content: 'Input', direction: 'top'}];
}

function buildMenuItems(
  nodeType: 'source' | 'multisource' | 'modification',
  devMode: boolean | undefined,
  onAddNode: (id: string) => void,
): m.Children[] {
  const nodes = nodeRegistry
    .list()
    .filter(([_id, descriptor]) => descriptor.type === nodeType)
    .filter(([_id, descriptor]) => !descriptor.devOnly || devMode);

  return buildCategorizedMenuItems(nodes, onAddNode);
}

function buildAddMenuItems(
  targetNode: QueryNode,
  onAddOperationNode: (id: string, node: QueryNode) => void,
): m.Children[] {
  const modificationItems = buildMenuItems('modification', undefined, (id) =>
    onAddOperationNode(id, targetNode),
  );
  const multisourceItems = buildMenuItems('multisource', undefined, (id) =>
    onAddOperationNode(id, targetNode),
  );

  // Add a divider between modification and multisource nodes if both exist
  if (modificationItems.length > 0 && multisourceItems.length > 0) {
    return [...modificationItems, m(MenuDivider), ...multisourceItems];
  }
  return [...modificationItems, ...multisourceItems];
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

  // A node is docked (not a root) if:
  // 1. It's a single-node operation (modification node)
  // 2. It has a prevNode (parent in the primary flow)
  // 3. It doesn't have a layout position (purely visual property)
  for (const node of allNodes) {
    if (
      singleNodeOperation(node.type) &&
      'prevNode' in node &&
      node.prevNode !== undefined &&
      isChildDocked(node, nodeLayouts)
    ) {
      dockedNodes.add(node);
    }
  }

  return allNodes.filter((n) => !dockedNodes.has(n));
}

function ensureNodeLayouts(
  roots: QueryNode[],
  attrs: GraphAttrs,
  nodeGraphApi: NodeGraphApi | null,
): void {
  // Assign layouts to new nodes using smart placement
  for (const qnode of roots) {
    if (!attrs.nodeLayouts.has(qnode.nodeId)) {
      let placement: Position;

      // Use NodeGraph API to find optimal non-overlapping placement
      if (nodeGraphApi) {
        // Create a simple node config without 'next' to get accurate placement
        // The 'next' property would include docked children and affect size calculation
        const noTopPort = isSourceNode(qnode) || isMultiSourceNode(qnode);
        const nodeTemplate: Omit<Node, 'x' | 'y'> = {
          id: qnode.nodeId,
          inputs: getInputLabels(qnode),
          outputs: [
            {
              content: 'Output',
              direction: 'bottom',
            },
          ],
          canDockBottom: true,
          canDockTop: !noTopPort,
          hue: getNodeHue(qnode),
          accentBar: true,
          content: m(NodeBox, {
            node: qnode,
            onDuplicateNode: attrs.onDuplicateNode,
            onDeleteNode: attrs.onDeleteNode,
            onAddOperationNode: attrs.onAddOperationNode,
          }),
          // Don't include 'next' here - we want placement for just this node
        };
        placement = nodeGraphApi.findPlacementForNode(nodeTemplate);
      } else {
        // Fallback to default position if API not ready yet
        placement = {
          x: LAYOUT_CONSTANTS.INITIAL_X,
          y: LAYOUT_CONSTANTS.INITIAL_Y,
        };
      }

      attrs.onNodeLayoutChange(qnode.nodeId, placement);
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
    const child = qnode.nextNodes[0];
    // Only dock the child if it's part of the primary flow chain
    // (i.e., the child's prevNode points back to this parent)
    if ('prevNode' in child && child.prevNode === qnode) {
      return renderChildNode(child, attrs);
    }
  }
  return undefined;
}

function createNodeConfig(
  qnode: QueryNode,
  attrs: GraphAttrs,
): Omit<Node, 'x' | 'y'> {
  const noTopPort = isSourceNode(qnode) || isMultiSourceNode(qnode);

  return {
    id: qnode.nodeId,
    inputs: getInputLabels(qnode),
    outputs: [
      {
        content: 'Output',
        direction: 'bottom',
        contextMenuItems: buildAddMenuItems(qnode, attrs.onAddOperationNode),
      },
    ],
    canDockBottom: true,
    canDockTop: !noTopPort,
    hue: getNodeHue(qnode),
    accentBar: true,
    content: m(NodeBox, {
      node: qnode,
      onDuplicateNode: attrs.onDuplicateNode,
      onDeleteNode: attrs.onDeleteNode,
      onAddOperationNode: attrs.onAddOperationNode,
    }),
    next: getNextDockedNode(qnode, attrs),
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

// For multi-source nodes, finds which input port (0-indexed) the parent is connected to
function calculateInputPort(child: QueryNode, parent: QueryNode): number {
  if (isMultiSourceNode(child)) {
    const index = child.prevNodes.indexOf(parent);
    return index !== -1 ? index : 0;
  }

  // Check if modification node has inputNodes (additional left-side inputs)
  if ('inputNodes' in child && 'prevNode' in child) {
    const modNode = child as ModificationNode;
    if (modNode.inputNodes !== undefined && Array.isArray(modNode.inputNodes)) {
      // Check if parent is the main prevNode (port 0)
      if (modNode.prevNode === parent) {
        return 0;
      }
      // Check if parent is in inputNodes array (ports 1+)
      const index = modNode.inputNodes.indexOf(parent);
      if (index !== -1) {
        return index + 1; // Port 1 = inputNodes[0], Port 2 = inputNodes[1], etc.
      }
    }
  }

  return 0;
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
      // But only skip if it's part of the primary flow chain (child's prevNode points back)
      if (
        qnode.nextNodes.length === 1 &&
        singleNodeOperation(child.type) &&
        isChildDocked(child, nodeLayouts) &&
        'prevNode' in child &&
        child.prevNode === qnode
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

  // For multisource nodes, all ports are left-side and 0-indexed (port 0 = prevNodes[0])
  // For modification nodes, port 0 is top (prevNode), ports 1+ are left-side (inputNodes[0], inputNodes[1], ...)
  let portIndex: number | undefined;
  if (isMultiSourceNode(toNode)) {
    portIndex = conn.toPort;
  } else {
    portIndex = conn.toPort > 0 ? conn.toPort - 1 : undefined;
  }
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
  private hasPerformedInitialLayout: boolean = false;

  private renderEmptyNodeGraph(attrs: GraphAttrs) {
    return m(EmptyGraph, {
      onAddSourceNode: attrs.onAddSourceNode,
      onImport: attrs.onImport,
    });
  }

  private renderControls(attrs: GraphAttrs) {
    const sourceMenuItems = buildMenuItems(
      'source',
      attrs.devMode,
      attrs.onAddSourceNode,
    );

    const modificationMenuItems = buildMenuItems(
      'modification',
      attrs.devMode,
      attrs.onAddSourceNode,
    );

    const operationMenuItems = buildMenuItems(
      'multisource',
      attrs.devMode,
      attrs.onAddSourceNode,
    );

    const addNodeMenuItems = [
      m(MenuTitle, {label: 'Sources'}),
      ...sourceMenuItems,
      m(MenuDivider),
      m(MenuTitle, {label: 'Operations'}),
      ...operationMenuItems,
      m(MenuDivider),
      m(MenuTitle, {label: 'Modification nodes'}),
      ...modificationMenuItems,
    ];

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
            label: 'Add Node',
            icon: Icons.Add,
            variant: ButtonVariant.Filled,
          }),
        },
        addNodeMenuItems,
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

    // Perform auto-layout if nodeLayouts is empty and API is available
    if (
      !this.hasPerformedInitialLayout &&
      this.nodeGraphApi &&
      attrs.nodeLayouts.size === 0 &&
      nodes.length > 0
    ) {
      this.hasPerformedInitialLayout = true;
      // Defer autoLayout to next tick to ensure DOM nodes are fully rendered
      setTimeout(() => {
        if (this.nodeGraphApi) {
          // Call autoLayout to arrange nodes hierarchically
          // autoLayout will call onNodeMove for each node it repositions
          this.nodeGraphApi.autoLayout();
        }
      }, 0);
    }

    return m(
      '.pf-exp-node-graph',
      {
        tabindex: 0,
      },
      [
        m(NodeGraph, {
          nodes,
          connections,
          selectedNodeIds: new Set(
            selectedNode?.nodeId ? [selectedNode.nodeId] : [],
          ),
          hideControls: true,
          onReady: (api: NodeGraphApi) => {
            this.nodeGraphApi = api;
          },
          multiselect: false,
          onNodeSelect: (nodeId: string) => {
            const qnode = findQueryNode(nodeId, rootNodes);
            if (qnode) {
              attrs.onNodeSelected(qnode);
            }
          },
          onSelectionClear: () => {
            attrs.onDeselect();
          },
          onNodeMove: (nodeId: string, x: number, y: number) => {
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
          onUndock: (
            _parentId: string,
            nodeId: string,
            x: number,
            y: number,
          ) => {
            // Store the new position in the layout map so node becomes independent
            attrs.onNodeLayoutChange(nodeId, {x, y});
            m.redraw();
          },
          onDock: (targetId: string, childNode: Omit<Node, 'x' | 'y'>) => {
            // Remove coordinates so node becomes "docked" (renders via parent's 'next')
            attrs.nodeLayouts.delete(childNode.id);

            // Create the connection between parent and child
            const parentNode = findQueryNode(targetId, rootNodes);
            const childQueryNode = findQueryNode(childNode.id, rootNodes);

            if (parentNode && childQueryNode) {
              // Add connection (this will update both nextNodes and prevNode/prevNodes)
              addConnection(parentNode, childQueryNode);
            }

            m.redraw();
          },
        } satisfies NodeGraphAttrs),
        this.renderControls(attrs),
      ],
    );
  }
}
