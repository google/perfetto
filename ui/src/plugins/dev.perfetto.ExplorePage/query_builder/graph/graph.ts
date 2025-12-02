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
  NodeType,
  addConnection,
  removeConnection,
} from '../../query_node';
import {EmptyGraph} from '../empty_graph';
import {nodeRegistry} from '../node_registry';
import {NodeBox} from './node_box';
import {buildCategorizedMenuItems} from './menu_utils';
import {getAllNodes, findNodeById} from '../graph_utils';

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
  BATCH_NODE_HORIZONTAL_OFFSET: 250,
};

// ========================================
// TYPE GUARDS
// ========================================

// Check if a node should show a top port based on its type (capabilities)
// rather than its current connection state
function shouldShowTopPort(node: QueryNode): boolean {
  // Single-input operation nodes always have a top port, even when disconnected
  return singleNodeOperation(node.type);
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

// Alias for consistency with existing code in this file
const findQueryNode = findNodeById;

// A node is "docked" if it has no layout (rendered as part of parent's chain via 'next' property)
function isChildDocked(child: QueryNode, nodeLayouts: LayoutMap): boolean {
  return !nodeLayouts.has(child.nodeId);
}

// ========================================
// NODE PORT AND MENU UTILITIES
// ========================================

function getInputLabels(node: QueryNode): NodePort[] {
  // Single-input operation nodes always have a top port (even when disconnected)
  if (singleNodeOperation(node.type)) {
    // Check if node also has secondaryInputs (like AddColumnsNode)
    if (node.secondaryInputs) {
      // Show both top port and side ports
      const labels: NodePort[] = [];
      labels.push({content: 'Input', direction: 'top'});

      // For AddColumnsNode, show exactly one left-side port
      if (node.type === NodeType.kAddColumns) {
        labels.push({content: 'Table', direction: 'left'});
      } else {
        // For other nodes with secondaryInputs (like FilterDuring) - show dynamic ports
        const numPorts = (node.secondaryInputs.connections.size ?? 0) + 1;
        for (let i = 0; i < numPorts; i++) {
          labels.push({content: `Input ${i}`, direction: 'left'});
        }
      }
      return labels;
    }

    // Single-input only (Sort, Filter, etc.) - show just top port
    return [{content: 'Input', direction: 'top'}];
  }

  // Multi-source nodes (IntervalIntersect, Merge, Union) - no primaryInput
  if (node.secondaryInputs) {
    // Check if node has custom input labels
    if ('getInputLabels' in node && typeof node.getInputLabels === 'function') {
      return (node as QueryNode & {getInputLabels: () => string[]})
        .getInputLabels()
        .map((label) => ({content: label, direction: 'left' as const}));
    }

    // Always show one extra empty port for adding new connections
    const numPorts = (node.secondaryInputs.connections.size ?? 0) + 1;
    const labels: NodePort[] = [];
    for (let i = 0; i < numPorts; i++) {
      labels.push({content: `Input ${i}`, direction: 'left'});
    }
    return labels;
  }

  // Source nodes have no inputs
  return [];
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
  // 2. It has a primaryInput (parent in the primary flow)
  // 3. It doesn't have a layout position (purely visual property)
  for (const node of allNodes) {
    if (
      singleNodeOperation(node.type) &&
      'primaryInput' in node &&
      node.primaryInput !== undefined &&
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
  let nodeOffset = 0;
  for (const qnode of roots) {
    if (!attrs.nodeLayouts.has(qnode.nodeId)) {
      let placement: Position;

      // Use NodeGraph API to find optimal non-overlapping placement
      if (nodeGraphApi) {
        // Create a simple node config without 'next' to get accurate placement
        // The 'next' property would include docked children and affect size calculation
        const canDockTop = shouldShowTopPort(qnode);
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
          canDockTop,
          hue: getNodeHue(qnode),
          accentBar: true,
          content: m(NodeBox, {
            node: qnode,
            onAddOperationNode: attrs.onAddOperationNode,
          }),
          // Don't include 'next' here - we want placement for just this node
        };
        placement = nodeGraphApi.findPlacementForNode(nodeTemplate);
      } else {
        // Fallback to default position if API not ready yet
        // Offset nodes horizontally by BATCH_NODE_HORIZONTAL_OFFSET
        // when multiple nodes are created in a batch to prevent overlap
        placement = {
          x:
            LAYOUT_CONSTANTS.INITIAL_X +
            nodeOffset * LAYOUT_CONSTANTS.BATCH_NODE_HORIZONTAL_OFFSET,
          y: LAYOUT_CONSTANTS.INITIAL_Y,
        };
        nodeOffset++;
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
    // (i.e., the child's primaryInput points back to this parent)
    if ('primaryInput' in child && child.primaryInput === qnode) {
      return renderChildNode(child, attrs);
    }
  }
  return undefined;
}

function buildNodeContextMenuItems(
  qnode: QueryNode,
  attrs: GraphAttrs,
): m.Children {
  return [
    m(MenuItem, {
      label: 'Duplicate',
      onclick: () => attrs.onDuplicateNode(qnode),
    }),
    m(MenuItem, {
      label: 'Delete',
      onclick: () => attrs.onDeleteNode(qnode),
    }),
  ];
}

function createNodeConfig(
  qnode: QueryNode,
  attrs: GraphAttrs,
): Omit<Node, 'x' | 'y'> {
  const canDockTop = shouldShowTopPort(qnode);

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
    canDockTop,
    hue: getNodeHue(qnode),
    accentBar: true,
    contextMenuItems: buildNodeContextMenuItems(qnode, attrs),
    content: m(NodeBox, {
      node: qnode,
      onAddOperationNode: attrs.onAddOperationNode,
    }),
    next: getNextDockedNode(qnode, attrs),
    invalid: !qnode.validate(),
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

// Single-input nodes use port 0 for primaryInput, multi-source nodes don't have primaryInput
function hasPrimaryInputPort(node: QueryNode): boolean {
  return singleNodeOperation(node.type);
}

// Find which visual port a parent node is connected to
function getInputPort(child: QueryNode, parent: QueryNode): number {
  if (child.primaryInput === parent) {
    return 0;
  }
  if (child.secondaryInputs) {
    const offset = hasPrimaryInputPort(child) ? 1 : 0;
    for (const [index, node] of child.secondaryInputs.connections) {
      if (node === parent) {
        return index + offset;
      }
    }
  }
  return 0;
}

// Convert visual port to secondary input index (undefined means primary input)
function toSecondaryIndex(
  node: QueryNode,
  visualPort: number,
): number | undefined {
  if (hasPrimaryInputPort(node)) {
    return visualPort === 0 ? undefined : visualPort - 1;
  }
  return visualPort;
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
      // But only skip if it's part of the primary flow chain (child's primaryInput points back)
      if (
        qnode.nextNodes.length === 1 &&
        singleNodeOperation(child.type) &&
        isChildDocked(child, nodeLayouts) &&
        'primaryInput' in child &&
        child.primaryInput === qnode
      ) {
        continue;
      }

      connections.push({
        fromNode: qnode.nodeId,
        fromPort: 0,
        toNode: child.nodeId,
        toPort: getInputPort(child, qnode),
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

  const secondaryIndex = toSecondaryIndex(toNode, conn.toPort);
  addConnection(fromNode, toNode, secondaryIndex);

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
              // Add connection (this will update both nextNodes and primaryInput/secondaryInputs)
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
