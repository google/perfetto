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
import {uuidv4} from '../../../../base/uuid';
import {
  MenuItem,
  MenuDivider,
  MenuTitle,
  PopupMenu,
} from '../../../../widgets/menu';
import {
  Connection,
  Label,
  Node,
  NodeGraph,
  NodeGraphApi,
  NodeGraphAttrs,
  NodePort,
} from '../../../../widgets/nodegraph';
import {createEditableTextLabels} from './text_label';
import {QueryNode, singleNodeOperation, NodeType} from '../../query_node';
import {NodeBox} from './node_box';
import {buildMenuItems} from './menu_utils';
import {
  getAllNodes,
  findNodeById,
  addConnection,
  removeConnection,
} from '../graph_utils';
import {RoundActionButton} from '../widgets';

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
  readonly labels?: ReadonlyArray<TextLabelData>;
  readonly onNodeSelected: (node: QueryNode) => void;
  readonly onDeselect: () => void;
  readonly onNodeLayoutChange: (nodeId: string, layout: Position) => void;
  readonly onLabelsChange?: (labels: TextLabelData[]) => void;
  readonly onAddSourceNode: (id: string) => void;
  readonly onAddOperationNode: (id: string, node: QueryNode) => void;
  readonly onClearAllNodes: () => void;
  readonly onDuplicateNode: (node: QueryNode) => void;
  readonly onDeleteNode: (node: QueryNode) => void;
  readonly onConnectionRemove: (
    fromNode: QueryNode,
    toNode: QueryNode,
    isSecondaryInput: boolean,
  ) => void;
  readonly onImport: () => void;
  readonly onExport: () => void;
  readonly onRecenterReady?: (recenter: () => void) => void;
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

// Calculate the number of ports to display for secondary inputs.
// Shows one extra empty port for adding new connections, but respects max limit.
function calculateNumPorts(
  currentConnections: number,
  max: number | 'unbounded',
): number {
  return max === 'unbounded'
    ? currentConnections + 1
    : Math.min(currentConnections + 1, max);
}

function getInputLabels(node: QueryNode): NodePort[] {
  // Single-input operation nodes always have a top port (even when disconnected)
  if (singleNodeOperation(node.type)) {
    const labels: NodePort[] = [];
    labels.push({content: 'Input', direction: 'top'});

    // Check if node also has secondaryInputs (like AddColumnsNode or FilterDuring)
    if (node.secondaryInputs) {
      // Show side ports using the node's custom port names
      const portNames = node.secondaryInputs.portNames;
      const currentConnections = node.secondaryInputs.connections.size ?? 0;
      const numPorts = calculateNumPorts(
        currentConnections,
        node.secondaryInputs.max,
      );

      for (let i = 0; i < numPorts; i++) {
        const portName = getPortName(portNames, i);
        labels.push({content: portName, direction: 'left'});
      }
    }
    return labels;
  }

  // Multi-source nodes (IntervalIntersect, Join, Union) - no primaryInput
  if (node.secondaryInputs) {
    const portNames = node.secondaryInputs.portNames;
    const currentConnections = node.secondaryInputs.connections.size ?? 0;
    const numPorts = calculateNumPorts(
      currentConnections,
      node.secondaryInputs.max,
    );
    const labels: NodePort[] = [];

    for (let i = 0; i < numPorts; i++) {
      const portName = getPortName(portNames, i);
      labels.push({content: portName, direction: 'left'});
    }
    return labels;
  }

  // Source nodes have no inputs
  return [];
}

// Helper function to get port name from either an array or a function
function getPortName(
  portNames: string[] | ((portIndex: number) => string),
  portIndex: number,
): string {
  if (typeof portNames === 'function') {
    return portNames(portIndex);
  }

  // Array of names - use the index or fallback if out of bounds
  return portNames[portIndex] ?? `Input ${portIndex}`;
}

function buildAddMenuItems(
  targetNode: QueryNode,
  onAddOperationNode: (id: string, node: QueryNode) => void,
): m.Children[] {
  const multisourceItems = buildMenuItems('multisource', (id) =>
    onAddOperationNode(id, targetNode),
  );
  const modificationItems = buildMenuItems('modification', (id) =>
    onAddOperationNode(id, targetNode),
  );

  return [
    m(MenuTitle, {label: 'Modification nodes'}),
    ...modificationItems,
    m(MenuDivider),
    m(MenuTitle, {label: 'Operations'}),
    ...multisourceItems,
  ];
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
  if (!nodeGraphApi) return;

  let lastPlacement: Position | undefined;

  for (const qnode of roots) {
    if (!attrs.nodeLayouts.has(qnode.nodeId)) {
      let placement: Position;

      if (!lastPlacement) {
        // First node - use API placement
        const canDockTop = shouldShowTopPort(qnode);
        const nodeTemplate: Omit<Node, 'x' | 'y'> = {
          id: qnode.nodeId,
          inputs: getInputLabels(qnode),
          outputs: [{content: 'Output', direction: 'bottom'}],
          canDockBottom: true,
          canDockTop,
          hue: getNodeHue(qnode),
          accentBar: true,
          content: m(NodeBox, {
            node: qnode,
            onAddOperationNode: attrs.onAddOperationNode,
          }),
        };
        placement = nodeGraphApi.findPlacementForNode(nodeTemplate);
      } else {
        // Subsequent nodes - place to the right of previous
        placement = {
          x: lastPlacement.x + LAYOUT_CONSTANTS.BATCH_NODE_HORIZONTAL_OFFSET,
          y: lastPlacement.y,
        };
      }

      lastPlacement = placement;
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
    case NodeType.kTimeRangeSource:
      return 33; // Orange (#ffe0b2)
    case NodeType.kAggregation:
      return 339; // Pink (#f8bbd0)
    case NodeType.kModifyColumns:
      return 261; // Purple (#d1c4e9)
    case NodeType.kAddColumns:
      return 232; // Indigo (#c5cae9)
    case NodeType.kFilterDuring:
      return 88; // Light Green (#dcedc8)
    case NodeType.kLimitAndOffset:
      return 175; // Teal (#b2dfdb)
    case NodeType.kSort:
      return 54; // Yellow (#fff9c4)
    case NodeType.kFilter:
      return 207; // Blue (#bbdefb)
    case NodeType.kIntervalIntersect:
      return 45; // Amber/Orange (#ffecb3)
    case NodeType.kUnion:
      return 187; // Cyan (#b2ebf2)
    case NodeType.kJoin:
      return 14; // Deep Orange (#ffccbc)
    case NodeType.kCreateSlices:
      return 100; // Green (#c8e6c9)
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
    console.warn(
      `Cannot create connection: node not found (from: ${conn.fromNode}, to: ${conn.toNode})`,
    );
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
  onConnectionRemove: (
    fromNode: QueryNode,
    toNode: QueryNode,
    isSecondaryInput: boolean,
  ) => void,
): void {
  const fromNode = findQueryNode(conn.fromNode, rootNodes);
  const toNode = findQueryNode(conn.toNode, rootNodes);

  if (!fromNode || !toNode) {
    console.warn(
      `Cannot remove connection: node not found (from: ${conn.fromNode}, to: ${conn.toNode})`,
    );
    return;
  }

  // Check BEFORE removal if this is a secondary input connection
  let isSecondaryInput = false;
  if (toNode.secondaryInputs?.connections) {
    for (const node of toNode.secondaryInputs.connections.values()) {
      if (node === fromNode) {
        isSecondaryInput = true;
        break;
      }
    }
  }

  // Use the helper function to cleanly remove the connection
  removeConnection(fromNode, toNode);

  // Call the parent callback for any additional cleanup (e.g., state management)
  onConnectionRemove(fromNode, toNode, isSecondaryInput);
}

// ========================================
// TEXT LABEL SERIALIZATION
// ========================================

/**
 * Serializable representation of a text label.
 * This interface contains only plain data types that can be safely
 * serialized to/from JSON, unlike the Label interface which includes
 * Mithril vnodes in the content field.
 */
export interface TextLabelData {
  readonly id: string;
  readonly x: number;
  readonly y: number;
  readonly width: number;
  readonly text: string;
}

// ========================================
// GRAPH COMPONENT
// ========================================

export class Graph implements m.ClassComponent<GraphAttrs> {
  private nodeGraphApi: NodeGraphApi | null = null;
  private hasPerformedInitialLayout: boolean = false;
  private hasPerformedInitialRecenter: boolean = false;
  private recenterRequired: boolean = false;
  private labels: Label[] = [];
  private labelTexts: Map<string, string> = new Map();
  private editingLabels: Set<string> = new Set();

  oninit(vnode: m.Vnode<GraphAttrs>) {
    // Load initial labels from attrs if provided
    if (vnode.attrs.labels) {
      this.deserializeLabels(vnode.attrs.labels as TextLabelData[]);
    }
  }

  onbeforeupdate(vnode: m.Vnode<GraphAttrs>, old: m.VnodeDOM<GraphAttrs>) {
    // Only update labels if the reference changed (indicating external state update)
    if (vnode.attrs.labels !== old.attrs.labels && vnode.attrs.labels) {
      this.deserializeLabels(vnode.attrs.labels as TextLabelData[]);
    }
    return true;
  }

  /**
   * Notifies parent component that labels have changed.
   * Called after any label modification (add, move, resize, delete).
   */
  private notifyLabelsChanged(attrs: GraphAttrs) {
    if (attrs.onLabelsChange) {
      attrs.onLabelsChange(this.serializeLabels());
    }
  }

  private addLabel(attrs: GraphAttrs) {
    const id = uuidv4();
    // Offset from the last label if one exists, otherwise use default position
    const lastLabel = this.labels[this.labels.length - 1];
    const x = lastLabel !== undefined ? lastLabel.x + 30 : 100;
    const y = lastLabel !== undefined ? lastLabel.y + 30 : 100;
    this.labels.push({
      id,
      x,
      y,
      width: 200,
      content: undefined, // Will be set in view
    });
    this.labelTexts.set(id, 'New label');
    this.notifyLabelsChanged(attrs);
    m.redraw();
  }

  /**
   * Serializes the current text labels to a JSON-compatible format.
   * Returns an array of TextLabelData that can be stored or transmitted.
   */
  serializeLabels(): TextLabelData[] {
    return this.labels.map((label) => ({
      id: label.id,
      x: label.x,
      y: label.y,
      width: label.width,
      text: this.labelTexts.get(label.id) ?? '',
    }));
  }

  /**
   * Deserializes text labels from a JSON-compatible format.
   * Replaces the current labels with the deserialized data.
   */
  deserializeLabels(data: TextLabelData[]): void {
    this.labels = data.map((labelData) => ({
      id: labelData.id,
      x: labelData.x,
      y: labelData.y,
      width: labelData.width,
      content: undefined, // Will be set in view
    }));

    this.labelTexts.clear();
    for (const labelData of data) {
      this.labelTexts.set(labelData.id, labelData.text);
    }

    this.editingLabels.clear();
    m.redraw();
  }

  private renderControls(attrs: GraphAttrs) {
    const sourceMenuItems = buildMenuItems('source', attrs.onAddSourceNode);

    const modificationMenuItems = buildMenuItems(
      'modification',
      attrs.onAddSourceNode,
    );

    const operationMenuItems = buildMenuItems(
      'multisource',
      attrs.onAddSourceNode,
    );

    const addNodeMenuItems = [
      m(MenuTitle, {label: 'Sources'}),
      ...sourceMenuItems,
      m(MenuDivider),
      m(MenuTitle, {label: 'Operations'}),
      ...operationMenuItems,
      m(MenuTitle, {label: 'Modification nodes'}),
      ...modificationMenuItems,
      m(MenuDivider),
      m(MenuItem, {
        label: 'Label',
        onclick: () => this.addLabel(attrs),
      }),
    ];

    const moreMenuItems = [
      m(MenuItem, {
        label: 'Export to JSON',
        icon: Icons.Download,
        onclick: attrs.onExport,
      }),
      m(MenuItem, {
        label: 'Import from JSON',
        icon: 'file_upload',
        onclick: attrs.onImport,
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
          trigger: RoundActionButton({
            icon: Icons.Add,
            title: 'Add Node',
            onclick: () => {},
          }),
        },
        addNodeMenuItems,
      ),
      m(Button, {
        icon: 'center_focus_strong',
        variant: ButtonVariant.Minimal,
        title: 'Center Graph',
        onclick: () => {
          if (this.nodeGraphApi) {
            this.nodeGraphApi.recenter();
          }
        },
      }),
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            icon: Icons.ContextMenuAlt,
            variant: ButtonVariant.Minimal,
          }),
        },
        moreMenuItems,
      ),
    );
  }

  view({attrs}: m.CVnode<GraphAttrs>) {
    const {rootNodes, selectedNode} = attrs;

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
      this.hasPerformedInitialRecenter = true;
      // Call autoLayout to arrange nodes hierarchically
      // autoLayout will call onNodeMove for each node it repositions
      this.nodeGraphApi.autoLayout();
      // Recenter will happen in the onReady callback after the next render
      this.recenterRequired = true;
    } else if (
      !this.hasPerformedInitialRecenter &&
      this.nodeGraphApi &&
      nodes.length > 0
    ) {
      // Recenter on first render even if auto-layout didn't run
      // (e.g., when loading from localStorage with existing positions)
      this.hasPerformedInitialRecenter = true;
      this.recenterRequired = true;
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
          fillHeight: true,
          onReady: (api: NodeGraphApi) => {
            this.nodeGraphApi = api;

            // Check if recenter is required and execute it after render
            if (this.recenterRequired) {
              this.nodeGraphApi.recenter();
              this.recenterRequired = false;
            }

            // Expose recenter function to parent component
            attrs.onRecenterReady?.(() => {
              if (this.nodeGraphApi) {
                this.nodeGraphApi.recenter();
              }
            });
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
            const parentNode = findQueryNode(targetId, rootNodes);
            const childQueryNode = findQueryNode(childNode.id, rootNodes);

            if (!parentNode || !childQueryNode) {
              console.warn('Cannot dock: parent or child node not found');
              m.redraw();
              return;
            }

            const existingChildren = parentNode.nextNodes;

            // Only allow docking if:
            // 1. Parent has no children, OR
            // 2. Parent has exactly one child and it's the child being docked (re-docking)
            const canDock =
              existingChildren.length === 0 ||
              (existingChildren.length === 1 &&
                existingChildren[0] === childQueryNode);

            if (!canDock) {
              console.warn('Cannot dock: parent already has children');
              m.redraw();
              return;
            }

            // Check if child can be docked (single-node operation)
            if (!singleNodeOperation(childQueryNode.type)) {
              console.warn(
                'Cannot dock: only single-node operations can be docked',
              );
              m.redraw();
              return;
            }

            // Dock the child
            attrs.nodeLayouts.delete(childNode.id);
            addConnection(parentNode, childQueryNode);
            m.redraw();
          },
          contextMenuOnHover: true,
          labels: createEditableTextLabels(
            this.labels,
            this.labelTexts,
            this.editingLabels,
            () => this.notifyLabelsChanged(attrs),
          ),
          onLabelMove: (labelId: string, x: number, y: number) => {
            const label = this.labels.find((l) => l.id === labelId);
            if (label) {
              label.x = x;
              label.y = y;
              this.notifyLabelsChanged(attrs);
            }
          },
          onLabelResize: (labelId: string, width: number) => {
            const label = this.labels.find((l) => l.id === labelId);
            if (label) {
              label.width = width;
              this.notifyLabelsChanged(attrs);
            }
          },
          onLabelRemove: (labelId: string) => {
            const labelIndex = this.labels.findIndex((l) => l.id === labelId);
            if (labelIndex !== -1) {
              this.labels.splice(labelIndex, 1);
            }
            this.labelTexts.delete(labelId);
            this.notifyLabelsChanged(attrs);
            m.redraw();
          },
        } satisfies NodeGraphAttrs),
        this.renderControls(attrs),
      ],
    );
  }
}
