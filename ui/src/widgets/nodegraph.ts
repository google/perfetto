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
 * A component for displaying and interacting with a node-based graph.
 *
 * Features:
 * - Draggable, selectable, and removable nodes.
 * - Pannable and zoomable canvas.
 * - Connectable ports to create links between nodes.
 * - Docking nodes to each other to form chains.
 * - Customizable node content and appearance.
 * - Auto-layout and fit-to-screen functionality.
 *
 * Minimal example:
 *
 * ```typescript
 * const nodes: Node[] = [
 *   {id: 'node1', x: 50, y: 50, outputs: [{direction: 'right'}]},
 *   {id: 'node2', x: 250, y: 50, inputs: [{direction: 'left'}]},
 * ];
 *
 * const connections: Connection[] = [
 *   {fromNode: 'node1', fromPort: 0, toNode: 'node2', toPort: 0},
 * ];
 *
 * m(NodeGraph, {
 *   nodes,
 *   connections,
 *   onConnect: (newConnection) => {
 *     // Handle new connection
 *   },
 *   onNodeMove: (nodeId, x, y) => {
 *     // Handle node position change (called when node is dropped)
 *   },
 * });
 * ```
 */
import m from 'mithril';
import {Button, ButtonVariant} from './button';
import {Icon} from './icon';
import {PopupMenu} from './menu';
import {classNames} from '../base/classnames';
import {Icons} from '../base/semantic_icons';

// Default height estimate for labels (used for box selection calculations)
const DEFAULT_LABEL_MIN_HEIGHT = 30;

// Typical height estimate for labels with content (used for autofit calculations)
// Labels can vary in height based on content, but this provides a reasonable
// estimate for bounding box calculations when actual DOM measurements aren't available
const TYPICAL_LABEL_HEIGHT = 100;

interface Position {
  x: number;
  y: number;
  transformedX?: number;
  transformedY?: number;
}

export interface Connection {
  readonly fromNode: string;
  readonly fromPort: number;
  readonly toNode: string;
  readonly toPort: number;
}

export interface NodeTitleBar {
  readonly title: m.Children;
}

export interface NodePort {
  readonly content?: m.Children;
  readonly direction: 'top' | 'left' | 'right' | 'bottom';
  readonly contextMenuItems?: m.Children;
}

export type DockedNode = Omit<Node, 'x' | 'y'>;

export interface Node {
  readonly id: string;
  readonly x: number;
  readonly y: number;
  readonly hue?: number; // Color of the title / accent bar (0-360)
  readonly accentBar?: boolean; // Optional strip of accent color on the left side (doesn't work well with titleBar)
  readonly titleBar?: NodeTitleBar; // Optional title bar (doesn't work well with accentBar or docking)
  readonly inputs?: ReadonlyArray<NodePort>;
  readonly outputs?: ReadonlyArray<NodePort>;
  readonly content?: m.Children; // Optional custom content to render in node body
  readonly next?: DockedNode; // Next node in chain
  readonly canDockTop?: boolean;
  readonly canDockBottom?: boolean;
  readonly contextMenuItems?: m.Children;
  readonly invalid?: boolean; // Whether this node is in an invalid state
}

export interface Label {
  readonly id: string;
  x: number;
  y: number;
  width: number; // Width of the label box (user can resize)
  content?: m.Children; // Content to render inside the label (optional, defaults to empty)
  selectable?: boolean; // Whether clicking the label selects it (default: false, only shift+click works)
}

interface ConnectingState {
  nodeId: string;
  portIndex: number;
  type: 'input' | 'output';
  portType: 'top' | 'bottom' | 'left' | 'right';
  x: number;
  y: number;
  transformedX: number;
  transformedY: number;
}

interface UndockCandidate {
  nodeId: string;
  parentId: string;
  startX: number;
  startY: number;
  renderY: number;
}

interface UndockedNode {
  nodeId: string;
  parentId: string;
}

interface SelectionRect {
  startX: number;
  startY: number;
  currentX: number;
  currentY: number;
}

interface CanvasState {
  draggedNode: string | null;
  dragOffset: Position;
  connecting: ConnectingState | null;
  mousePos: Position;
  selectedNodes: ReadonlySet<string>;
  panOffset: Position;
  isPanning: boolean;
  panStart: Position;
  zoom: number;
  dockTarget: string | null; // Node being targeted for docking
  isDockZone: boolean; // Whether we're in valid dock position
  undockCandidate: UndockCandidate | null; // Tracks potential undock before threshold
  undockedNode: UndockedNode | null; // Node that was undocked (set when threshold exceeded)
  hoveredPort: {
    nodeId: string;
    portIndex: number;
    type: 'input' | 'output';
  } | null;
  selectionRect: SelectionRect | null; // Box selection state
  canvasMouseDownPos: Position;
  tempNodePositions: Map<string, Position>; // Temporary positions during drag
  tempLabelPositions: Map<string, Position>; // Temporary label positions during drag
  tempLabelWidths: Map<string, number>; // Temporary label widths during resize
  draggedLabel: string | null; // ID of label being dragged
  labelDragStartPos: Position | null; // Position where label drag started
  resizingLabel: string | null; // ID of label being resized
  resizeStartWidth: number; // Width when resize started
  resizeStartX: number; // Mouse X position when resize started
}

export interface NodeGraphApi {
  autoLayout: () => void;
  recenter: () => void;
  findPlacementForNode: (node: Omit<Node, 'x' | 'y'>) => Position;
}

export interface NodeGraphAttrs {
  readonly nodes: ReadonlyArray<Node>;
  readonly connections: ReadonlyArray<Connection>;
  readonly labels?: ReadonlyArray<Label>;
  readonly onConnect?: (connection: Connection) => void;
  readonly onNodeMove?: (nodeId: string, x: number, y: number) => void;
  readonly onConnectionRemove?: (index: number) => void;
  readonly onReady?: (api: NodeGraphApi) => void;
  // Selection state and callbacks apply to both nodes and labels.
  // selectedNodeIds should contain IDs of both selected nodes and labels.
  readonly selectedNodeIds?: ReadonlySet<string>;
  // Called when a node or label is selected (replacing current selection).
  readonly onNodeSelect?: (nodeId: string) => void;
  // Called when a node or label is added to the current selection (multiselect).
  readonly onNodeAddToSelection?: (nodeId: string) => void;
  // Called when a node or label is removed from the current selection.
  readonly onNodeRemoveFromSelection?: (nodeId: string) => void;
  readonly onSelectionClear?: () => void;
  readonly onDock?: (
    parentId: string,
    childNode: Omit<Node, 'x' | 'y'>,
  ) => void;
  readonly onUndock?: (
    parentId: string,
    nodeId: string,
    x: number,
    y: number,
  ) => void;
  readonly onNodeRemove?: (nodeId: string) => void;
  readonly onLabelMove?: (labelId: string, x: number, y: number) => void;
  readonly onLabelResize?: (labelId: string, width: number) => void;
  readonly onLabelRemove?: (labelId: string) => void;
  readonly hideControls?: boolean;
  readonly multiselect?: boolean; // Enable multi-node selection (default: true)
  readonly contextMenuOnHover?: boolean; // Show context menu on hover (default: false)
  readonly fillHeight?: boolean;
  readonly toolbarItems?: m.Children;
  readonly style?: Partial<CSSStyleDeclaration>;
}

const UNDOCK_THRESHOLD = 5; // Pixels to drag before undocking

function isPortConnected(
  nodeId: string,
  portType: 'input' | 'output',
  portIndex: number,
  connections: ReadonlyArray<Connection>,
): boolean {
  return connections.some((conn) => {
    if (portType === 'input') {
      return conn.toNode === nodeId && conn.toPort === portIndex;
    } else {
      return conn.fromNode === nodeId && conn.fromPort === portIndex;
    }
  });
}

// Get the entire chain starting from a root node
function getChain(rootNode: Node): Array<Node | Omit<Node, 'x' | 'y'>> {
  const chain: Array<Node | Omit<Node, 'x' | 'y'>> = [rootNode];
  let current = rootNode.next;

  while (current) {
    chain.push(current);
    current = current.next;
  }

  return chain;
}

function createCurve(
  x1: number,
  y1: number,
  x2: number,
  y2: number,
  fromPortType?: 'top' | 'bottom' | 'left' | 'right',
  toPortType?: 'top' | 'bottom' | 'left' | 'right',
  shortenEnd = 0,
): string {
  const dx = x2 - x1;
  const dy = y2 - y1;
  const distance = Math.sqrt(dx * dx + dy * dy);

  let cx1: number;
  let cy1: number;
  let cx2: number;
  let cy2: number;

  if (shortenEnd > 0) {
    if (toPortType === 'bottom') {
      y2 += shortenEnd;
    } else if (toPortType === 'top') {
      y2 -= shortenEnd;
    } else if (toPortType === 'left') {
      x2 -= shortenEnd;
    } else if (toPortType === 'right') {
      x2 += shortenEnd;
    }
  }

  // For top/bottom ports, control points extend vertically
  // For left/right ports, control points extend horizontally
  if (fromPortType === 'bottom' || fromPortType === 'top') {
    // First control point extends vertically
    const verticalOffset = Math.max(Math.abs(dy) * 0.5, distance * 0.5);
    cx1 = x1;
    cy1 = fromPortType === 'bottom' ? y1 + verticalOffset : y1 - verticalOffset;
  } else {
    // First control point extends horizontally for left/right ports
    const horizontalOffset = Math.max(Math.abs(dx) * 0.5, distance * 0.5);
    cx1 = x1 + horizontalOffset;
    cy1 = y1; // Keep Y constant for horizontal extension
  }

  if (toPortType === 'bottom' || toPortType === 'top') {
    // Second control point extends vertically
    const verticalOffset = Math.max(Math.abs(dy) * 0.5, distance * 0.5);
    cx2 = x2;
    cy2 = toPortType === 'bottom' ? y2 + verticalOffset : y2 - verticalOffset;
  } else {
    // Second control point extends horizontally for left/right ports
    const horizontalOffset = Math.max(Math.abs(dx) * 0.5, distance * 0.5);
    cx2 = x2 - horizontalOffset;
    cy2 = y2; // Keep Y constant for horizontal extension
  }

  // if (shortenEnd > 0) {
  //   const tangentX = x2 - cx2;
  //   const tangentY = y2 - cy2;
  //   const tangentLength = Math.sqrt(tangentX * tangentX + tangentY * tangentY);
  //   if (tangentLength > shortenEnd) {
  //     const unitTangentX = tangentX / tangentLength;
  //     const unitTangentY = tangentY / tangentLength;
  //     x2 -= unitTangentX * shortenEnd;
  //     y2 -= unitTangentY * shortenEnd;
  //   }
  // }

  return `M ${x1} ${y1} C ${cx1} ${cy1}, ${cx2} ${cy2}, ${x2} ${y2}`;
}

export function NodeGraph(): m.Component<NodeGraphAttrs> {
  const canvasState: CanvasState = {
    draggedNode: null,
    dragOffset: {x: 0, y: 0},
    connecting: null,
    mousePos: {x: 0, y: 0},
    selectedNodes: new Set<string>(),
    panOffset: {x: 0, y: 0},
    isPanning: false,
    panStart: {x: 0, y: 0},
    zoom: 1.0,
    dockTarget: null,
    isDockZone: false,
    undockCandidate: null,
    undockedNode: null,
    hoveredPort: null,
    selectionRect: null,
    canvasMouseDownPos: {x: 0, y: 0},
    tempNodePositions: new Map<string, Position>(),
    tempLabelPositions: new Map<string, Position>(),
    tempLabelWidths: new Map<string, number>(),
    draggedLabel: null,
    labelDragStartPos: null,
    resizingLabel: null,
    resizeStartWidth: 0,
    resizeStartX: 0,
  };

  // Track drag state for batching updates
  let dragStartPosition: {nodeId: string; x: number; y: number} | null = null;
  let currentDragPosition: {x: number; y: number} | null = null;

  let latestVnode: m.Vnode<NodeGraphAttrs> | null = null;
  let canvasElement: HTMLElement | null = null;

  // API functions that are exposed to parent components via onReady callback
  // These are initialized in oncreate and can be used in subsequent lifecycle hooks
  let autoLayoutApi: (() => void) | null = null;
  let recenterApi: (() => void) | null = null;
  let findPlacementForNodeApi:
    | ((newNode: Omit<Node, 'x' | 'y'>) => Position)
    | null = null;

  const handleMouseMove = (e: PointerEvent) => {
    m.redraw();
    if (!latestVnode || !canvasElement) return;
    const vnode = latestVnode;
    const canvas = canvasElement;
    const canvasRect = canvas.getBoundingClientRect();

    // Store both screen and transformed coordinates
    canvasState.mousePos = {
      x: e.clientX,
      y: e.clientY,
      transformedX:
        (e.clientX - canvasRect.left - canvasState.panOffset.x) /
        canvasState.zoom,
      transformedY:
        (e.clientY - canvasRect.top - canvasState.panOffset.y) /
        canvasState.zoom,
    };

    // Track hovered port (useful for connection snapping and visual feedback)
    const portElement = (e.target as HTMLElement).closest('.pf-port.pf-input');
    if (portElement) {
      const nodeElement = portElement.closest(
        '[data-node]',
      ) as HTMLElement | null;
      const portId =
        portElement.getAttribute('data-port') ||
        portElement.parentElement?.getAttribute('data-port');

      if (nodeElement && portId) {
        const nodeId = nodeElement.dataset.node!;
        const [type, portIndexStr] = portId.split('-');
        if (type === 'input') {
          const portIndex = parseInt(portIndexStr, 10);
          canvasState.hoveredPort = {nodeId, portIndex, type: 'input'};
        } else {
          canvasState.hoveredPort = null;
        }
      } else {
        canvasState.hoveredPort = null;
      }
    } else {
      canvasState.hoveredPort = null;
    }

    if (canvasState.selectionRect) {
      // Update selection rectangle
      canvasState.selectionRect.currentX =
        canvasState.mousePos.transformedX ?? 0;
      canvasState.selectionRect.currentY =
        canvasState.mousePos.transformedY ?? 0;
      m.redraw();
    } else if (canvasState.draggedLabel !== null) {
      // Handle label dragging - store temp position, don't call callback yet
      const newX =
        (canvasState.mousePos.transformedX ?? 0) - canvasState.dragOffset.x;
      const newY =
        (canvasState.mousePos.transformedY ?? 0) - canvasState.dragOffset.y;

      // Store temporary position during drag
      canvasState.tempLabelPositions.set(canvasState.draggedLabel, {
        x: newX,
        y: newY,
      });
      m.redraw();
    } else if (canvasState.resizingLabel !== null) {
      // Handle label resizing - store temp width, don't call callback yet
      const currentX = canvasState.mousePos.transformedX ?? 0;
      const deltaX = currentX - canvasState.resizeStartX;
      const newWidth = Math.max(100, canvasState.resizeStartWidth + deltaX);

      // Store temporary width during resize
      canvasState.tempLabelWidths.set(canvasState.resizingLabel, newWidth);
      m.redraw();
    } else if (canvasState.isPanning) {
      // Pan the canvas
      const dx = e.clientX - canvasState.panStart.x;
      const dy = e.clientY - canvasState.panStart.y;
      canvasState.panOffset = {
        x: canvasState.panOffset.x + dx,
        y: canvasState.panOffset.y + dy,
      };
      canvasState.panStart = {x: e.clientX, y: e.clientY};
      m.redraw();
    } else if (canvasState.undockCandidate !== null) {
      // Check if we've exceeded the undock threshold
      const dx = e.clientX - canvasState.undockCandidate.startX;
      const dy = e.clientY - canvasState.undockCandidate.startY;
      const distance = Math.sqrt(dx * dx + dy * dy);

      if (distance > UNDOCK_THRESHOLD) {
        // Exceeded threshold - call onUndock immediately so node becomes independent
        const {onUndock} = vnode.attrs;
        const tempX =
          (canvasState.undockCandidate.startX -
            canvasRect.left -
            canvasState.panOffset.x) /
            canvasState.zoom -
          canvasState.dragOffset.x / canvasState.zoom;
        const tempY = canvasState.undockCandidate.renderY;

        // Store temp position for this node
        canvasState.tempNodePositions.set(canvasState.undockCandidate.nodeId, {
          x: tempX,
          y: tempY,
        });

        // Immediately call onUndock so the node becomes independent
        if (onUndock) {
          onUndock(
            canvasState.undockCandidate.parentId,
            canvasState.undockCandidate.nodeId,
            tempX,
            tempY,
          );
        }

        // Mark as undocked so we track it as a regular drag now
        canvasState.undockedNode = {
          nodeId: canvasState.undockCandidate.nodeId,
          parentId: canvasState.undockCandidate.parentId,
        };

        canvasState.undockCandidate = null;
        m.redraw(); // Force update so nodes array regenerates
      }
    } else if (canvasState.draggedNode !== null) {
      // Calculate new position relative to canvas container (accounting for pan and zoom)
      const newX =
        (e.clientX - canvasRect.left - canvasState.panOffset.x) /
          canvasState.zoom -
        canvasState.dragOffset.x / canvasState.zoom;
      const newY =
        (e.clientY - canvasRect.top - canvasState.panOffset.y) /
          canvasState.zoom -
        canvasState.dragOffset.y / canvasState.zoom;

      // Store current position internally
      currentDragPosition = {x: newX, y: newY};
      canvasState.tempNodePositions.set(canvasState.draggedNode, {
        x: newX,
        y: newY,
      });

      // Check if we're in a dock zone (exclude the parent we just undocked from)
      const {nodes} = vnode.attrs;
      const draggedNode = nodes.find((n) => n.id === canvasState.draggedNode);
      if (draggedNode) {
        const dockInfo = findDockTarget(draggedNode, newX, newY, nodes);
        canvasState.dockTarget = dockInfo.targetNodeId;
        canvasState.isDockZone = dockInfo.isValidZone;
      }
      m.redraw();
    }
  };

  const handleMouseUp = () => {
    if (!latestVnode) return;
    const vnode = latestVnode;

    // Handle box selection completion
    if (canvasState.selectionRect) {
      const {nodes = [], labels = []} = vnode.attrs;
      const rect = canvasState.selectionRect;
      const minX = Math.min(rect.startX, rect.currentX);
      const maxX = Math.max(rect.startX, rect.currentX);
      const minY = Math.min(rect.startY, rect.currentY);
      const maxY = Math.max(rect.startY, rect.currentY);

      // Helper to check if a node at given position overlaps with selection rectangle
      const nodeOverlapsRect = (
        nodeX: number,
        nodeY: number,
        nodeId: string,
      ): boolean => {
        const dims = getNodeDimensions(nodeId);
        const nodeRight = nodeX + dims.width;
        const nodeBottom = nodeY + dims.height;

        return (
          nodeX < maxX && nodeRight > minX && nodeY < maxY && nodeBottom > minY
        );
      };

      // Helper to check if a label overlaps with selection rectangle
      const labelOverlapsRect = (label: Label): boolean => {
        const labelRight = label.x + label.width;
        const labelBottom = label.y + DEFAULT_LABEL_MIN_HEIGHT;

        return (
          label.x < maxX &&
          labelRight > minX &&
          label.y < maxY &&
          labelBottom > minY
        );
      };

      // Find all nodes (including chained/docked nodes) that intersect with the selection rectangle
      const selectedInRect: string[] = [];
      nodes.forEach((node) => {
        // Check root node
        if (nodeOverlapsRect(node.x, node.y, node.id)) {
          selectedInRect.push(node.id);
        }

        // Check all chained nodes
        const chain = getChain(node);
        let currentY = node.y;
        chain.slice(1).forEach((chainNode) => {
          // For chained nodes, calculate their Y position
          const previousNodeId = chain[chain.indexOf(chainNode) - 1].id;
          currentY += getNodeDimensions(previousNodeId).height;

          if (nodeOverlapsRect(node.x, currentY, chainNode.id)) {
            selectedInRect.push(chainNode.id);
          }
        });
      });

      // Find all labels that intersect with the selection rectangle
      labels.forEach((label) => {
        if (labelOverlapsRect(label)) {
          selectedInRect.push(label.id);
        }
      });

      // Add all selected nodes and labels to selection
      const {onNodeAddToSelection} = vnode.attrs;
      selectedInRect.forEach((id) => {
        if (!canvasState.selectedNodes.has(id)) {
          if (onNodeAddToSelection !== undefined) {
            onNodeAddToSelection(id);
          }
        }
      });

      canvasState.selectionRect = null;
      m.redraw();
      return;
    }

    // Handle docking if in dock zone
    if (
      canvasState.draggedNode &&
      canvasState.isDockZone &&
      canvasState.dockTarget
    ) {
      const {nodes = [], onDock} = vnode.attrs;
      const draggedNode = nodes.find((n) => n.id === canvasState.draggedNode);
      if (onDock && draggedNode) {
        // Create child node without x/y coordinates
        // eslint-disable-next-line @typescript-eslint/no-unused-vars
        const {x, y, ...childNode} = draggedNode;
        onDock(canvasState.dockTarget, childNode);
      }
    }

    // Check for collision and finalize drag (only for non-docked/undocked nodes)
    if (canvasState.draggedNode !== null && !canvasState.isDockZone) {
      const {nodes = [], onNodeMove} = vnode.attrs;
      const draggedNode = nodes.find((n) => n.id === canvasState.draggedNode);

      // Only do overlap checking if NOT being docked
      if (draggedNode) {
        // Get actual node dimensions from DOM
        const dims = getNodeDimensions(draggedNode.id);

        // Calculate total height of the dragged node's chain
        const chain = getChain(draggedNode);
        let chainHeight = 0;
        chain.forEach((chainNode) => {
          chainHeight += getNodeDimensions(chainNode.id).height;
        });

        // Check if node (and its entire chain) overlaps with any other nodes
        if (
          currentDragPosition &&
          checkNodeOverlap(
            currentDragPosition.x,
            currentDragPosition.y,
            draggedNode.id,
            nodes,
            dims.width,
            chainHeight,
          )
        ) {
          // Find nearest non-overlapping position
          const newPos = findNearestNonOverlappingPosition(
            currentDragPosition.x,
            currentDragPosition.y,
            draggedNode.id,
            nodes,
            dims.width,
            chainHeight,
          );
          // Update to the non-overlapping position
          currentDragPosition = newPos;
          canvasState.tempNodePositions.set(draggedNode.id, newPos);
        }
      }

      // Call onNodeMove with final position if it changed
      // For undocked nodes, this provides the final position after dragging
      // For regular nodes, this is the only position update
      if (onNodeMove !== undefined && currentDragPosition !== null) {
        const startX = dragStartPosition?.x ?? 0;
        const startY = dragStartPosition?.y ?? 0;
        const moved =
          Math.abs(currentDragPosition.x - startX) > 0.5 ||
          Math.abs(currentDragPosition.y - startY) > 0.5;
        if (moved || canvasState.undockedNode !== null) {
          onNodeMove(
            canvasState.draggedNode,
            currentDragPosition.x,
            currentDragPosition.y,
          );
        }
      }
    }

    // Handle label callbacks with final values
    const {onLabelMove, onLabelResize} = vnode.attrs;

    if (canvasState.draggedLabel !== null) {
      const finalPos = canvasState.tempLabelPositions.get(
        canvasState.draggedLabel,
      );
      if (finalPos && onLabelMove) {
        onLabelMove(canvasState.draggedLabel, finalPos.x, finalPos.y);
      }
    }

    if (canvasState.resizingLabel !== null) {
      const finalWidth = canvasState.tempLabelWidths.get(
        canvasState.resizingLabel,
      );
      if (finalWidth !== undefined && onLabelResize) {
        onLabelResize(canvasState.resizingLabel, finalWidth);
      }
    }

    // Cleanup label state
    canvasState.draggedLabel = null;
    canvasState.labelDragStartPos = null;
    canvasState.resizingLabel = null;
    canvasState.tempLabelPositions.clear();
    canvasState.tempLabelWidths.clear();

    canvasState.draggedNode = null;
    dragStartPosition = null;
    currentDragPosition = null;
    canvasState.connecting = null;
    canvasState.hoveredPort = null;
    canvasState.isPanning = false;
    canvasState.dockTarget = null;
    canvasState.isDockZone = false;
    canvasState.undockCandidate = null;
    canvasState.undockedNode = null;
    canvasState.tempNodePositions.clear();
    m.redraw();
  };

  // Helper to determine port type based on port index
  function getPortType(
    nodeId: string,
    portType: 'input' | 'output',
    portIndex: number,
    nodes: ReadonlyArray<Node>,
  ): 'top' | 'bottom' | 'left' | 'right' {
    // Search in main nodes array
    let node: Node | Omit<Node, 'x' | 'y'> | undefined = nodes.find(
      (n) => n.id === nodeId,
    );

    // If not found, search in the next chains of all nodes
    if (!node) {
      for (const rootNode of nodes) {
        let current = rootNode.next;
        while (current) {
          if (current.id === nodeId) {
            node = current;
            break;
          }
          current = current.next;
        }
        if (node) break;
      }
    }

    if (!node) return portType === 'input' ? 'left' : 'right';

    // Get the port from the node
    const ports = portType === 'input' ? node.inputs : node.outputs;
    if (!ports || portIndex >= ports.length) {
      return portType === 'input' ? 'left' : 'right';
    }

    return ports[portIndex].direction;
  }

  function renderConnections(
    svg: SVGElement,
    connections: ReadonlyArray<Connection>,
    nodes: ReadonlyArray<Node>,
    onConnectionRemove?: (index: number) => void,
  ) {
    const shortenLength = 16;
    const arrowheadLength = 4;

    // Cache all port positions at once for performance
    const portPositionCache = new Map<string, Position>();

    // Query all ports in one go and cache their positions
    const allPorts = document.querySelectorAll('.pf-port[data-port]');
    allPorts.forEach((portElement) => {
      const portId = portElement.getAttribute('data-port');
      if (!portId) return;

      const nodeElement = portElement.closest(
        '[data-node]',
      ) as HTMLElement | null;
      if (!nodeElement) return;

      const nodeId = nodeElement.dataset.node;
      if (!nodeId) return;

      const [portType, portIndexStr] = portId.split('-');
      const cacheKey = `${nodeId}-${portType}-${portIndexStr}`;

      // Calculate position
      const chainContainer = nodeElement.closest(
        '.pf-node-wrapper',
      ) as HTMLElement | null;

      let nodeLeft: number;
      let nodeTop: number;

      if (chainContainer) {
        // Node is in a dock chain - use container's position
        nodeLeft = parseFloat(chainContainer.style.left) || 0;
        nodeTop = parseFloat(chainContainer.style.top) || 0;

        // Add offset of node within the chain
        const chainRect = chainContainer.getBoundingClientRect();
        const nodeRect = nodeElement.getBoundingClientRect();
        const offsetY = (nodeRect.top - chainRect.top) / canvasState.zoom;

        nodeTop += offsetY;
      } else {
        // Standalone node - use its position directly
        nodeLeft = parseFloat(nodeElement.style.left) || 0;
        nodeTop = parseFloat(nodeElement.style.top) || 0;
      }

      // Get port's position relative to the node
      const portRect = portElement.getBoundingClientRect();
      const nodeRect = nodeElement.getBoundingClientRect();

      // Calculate offset in screen space, then divide by zoom to get canvas content space
      const portX =
        (portRect.left - nodeRect.left + portRect.width / 2) / canvasState.zoom;
      const portY =
        (portRect.top - nodeRect.top + portRect.height / 2) / canvasState.zoom;

      portPositionCache.set(cacheKey, {
        x: nodeLeft + portX,
        y: nodeTop + portY,
      });
    });

    // Helper function to get port position from cache or fallback to direct lookup
    const getPortPos = (
      nodeId: string,
      portType: 'input' | 'output',
      portIndex: number,
    ): Position => {
      const cacheKey = `${nodeId}-${portType}-${portIndex}`;
      return (
        portPositionCache.get(cacheKey) ||
        getPortPosition(nodeId, portType, portIndex)
      );
    };

    // Build arrowhead markers using mithril
    const arrowheadMarker = (id: string) =>
      m(
        'marker',
        {
          id,
          viewBox: `0 0 ${arrowheadLength} 10`,
          refX: '0',
          refY: '5',
          markerWidth: `${arrowheadLength}`,
          markerHeight: '10',
          orient: 'auto',
        },
        m('polygon', {
          points: `0 2.5, ${arrowheadLength} 5, 0 7.5`,
          fill: 'context-stroke',
        }),
      );

    // Build connection paths using mithril
    // Each connection is rendered as two paths: a wider invisible hitbox and the visible line
    const connectionPaths = connections
      .map((conn, idx) => {
        const from = getPortPos(conn.fromNode, 'output', conn.fromPort);
        const to = getPortPos(conn.toNode, 'input', conn.toPort);

        // Validate that both ports exist (return {x: 0, y: 0} if not found)
        const fromValid = from.x !== 0 || from.y !== 0;
        const toValid = to.x !== 0 || to.y !== 0;

        if (!fromValid || !toValid) {
          console.warn(
            `Invalid connection: ${conn.fromNode}:${conn.fromPort} -> ${conn.toNode}:${conn.toPort}`,
            !fromValid ? `(source port not found)` : `(target port not found)`,
          );
          return null;
        }

        const fromPortType = getPortType(
          conn.fromNode,
          'output',
          conn.fromPort,
          nodes,
        );
        const toPortType = getPortType(
          conn.toNode,
          'input',
          conn.toPort,
          nodes,
        );

        const pathData = createCurve(
          from.x,
          from.y,
          to.x,
          to.y,
          fromPortType,
          toPortType,
          shortenLength,
        );

        const handlePointerDown = (e: PointerEvent) => {
          e.stopPropagation();
          e.preventDefault();
        };

        const handleClick = (e: Event) => {
          e.stopPropagation();
          if (onConnectionRemove !== undefined) {
            onConnectionRemove(idx);
          }
        };

        // Return a group with both the hitbox and visible path
        return m('g', {key: `conn-${idx}`, class: 'pf-connection-group'}, [
          // Invisible wider hitbox path
          m('path', {
            d: pathData,
            class: 'pf-connection-hitbox',
            style: {
              stroke: 'transparent',
              strokeWidth: '20',
              fill: 'none',
              pointerEvents: 'stroke',
              cursor: 'pointer',
            },
            onpointerdown: handlePointerDown,
            onclick: handleClick,
          }),
          // Visible connection path
          m('path', {
            'd': pathData,
            'class': 'pf-connection',
            'marker-end': 'url(#arrowhead)',
            'style': {
              pointerEvents: 'none',
            },
            'onpointerdown': handlePointerDown,
            'onclick': handleClick,
          }),
        ]);
      })
      .filter((path) => path !== null);

    // Build temp connection if connecting
    let tempConnectionPath = null;
    if (canvasState.connecting) {
      const fromX = canvasState.connecting.transformedX;
      const fromY = canvasState.connecting.transformedY;
      let toX = canvasState.mousePos.transformedX ?? 0;
      let toY = canvasState.mousePos.transformedY ?? 0;

      const fromPortType = canvasState.connecting.portType;
      let toPortType: 'top' | 'left' | 'right' | 'bottom' =
        fromPortType === 'top' || fromPortType === 'bottom' ? 'top' : 'left';

      if (
        canvasState.hoveredPort &&
        canvasState.connecting.type === 'output' &&
        canvasState.hoveredPort.type === 'input'
      ) {
        const {nodeId, portIndex, type} = canvasState.hoveredPort;
        const hoverPos = getPortPos(nodeId, type, portIndex);
        if (hoverPos.x !== 0 || hoverPos.y !== 0) {
          toX = hoverPos.x;
          toY = hoverPos.y;
          toPortType = getPortType(nodeId, type, portIndex, nodes);
        }
      }

      tempConnectionPath = m('path', {
        'class': 'pf-temp-connection',
        'd': createCurve(
          fromX,
          fromY,
          toX,
          toY,
          fromPortType,
          toPortType,
          shortenLength,
        ),
        'marker-end': 'url(#arrowhead)',
      });
    }

    // Render everything using mithril's render function
    m.render(svg, [
      m('defs', [arrowheadMarker('arrowhead')]),
      m('g', connectionPaths),
      tempConnectionPath,
    ]);
  }

  function getPortPosition(
    nodeId: string,
    portType: 'input' | 'output',
    portIndex: number,
  ): Position {
    // For port index 0 (top/bottom), data-port is on .pf-port itself
    // For port index 1+ (left/right), data-port is on .pf-port-row wrapper
    const selector =
      portIndex === 0
        ? `[data-node="${nodeId}"] .pf-port[data-port="${portType}-${portIndex}"]`
        : `[data-node="${nodeId}"] [data-port="${portType}-${portIndex}"] .pf-port`;

    const portElement = document.querySelector(selector);

    if (portElement) {
      const nodeElement = portElement.closest('.pf-node') as HTMLElement | null;
      if (nodeElement !== null) {
        // Check if node is in a dock chain (flexbox positioning)
        const chainContainer = nodeElement.closest(
          '.pf-node-wrapper',
        ) as HTMLElement | null;

        let nodeLeft: number;
        let nodeTop: number;

        if (chainContainer) {
          // Node is in a dock chain - use container's position
          nodeLeft = parseFloat(chainContainer.style.left) || 0;
          nodeTop = parseFloat(chainContainer.style.top) || 0;

          // Add offset of node within the chain
          const chainRect = chainContainer.getBoundingClientRect();
          const nodeRect = nodeElement.getBoundingClientRect();
          const offsetY = (nodeRect.top - chainRect.top) / canvasState.zoom;

          nodeTop += offsetY;
        } else {
          // Standalone node - use its position directly
          nodeLeft = parseFloat(nodeElement.style.left) || 0;
          nodeTop = parseFloat(nodeElement.style.top) || 0;
        }

        // Get port's position relative to the node
        const portRect = portElement.getBoundingClientRect();
        const nodeRect = nodeElement.getBoundingClientRect();

        // Calculate offset in screen space, then divide by zoom to get canvas content space
        const portX =
          (portRect.left - nodeRect.left + portRect.width / 2) /
          canvasState.zoom;
        const portY =
          (portRect.top - nodeRect.top + portRect.height / 2) /
          canvasState.zoom;

        return {
          x: nodeLeft + portX,
          y: nodeTop + portY,
        };
      }
    }

    return {x: 0, y: 0};
  }

  // Find if dragged node is in dock zone of any node
  function findDockTarget(
    draggedNode: Node,
    draggedX: number,
    draggedY: number,
    nodes: ReadonlyArray<Node>,
  ): {targetNodeId: string | null; isValidZone: boolean} {
    const DOCK_DISTANCE = 30;
    const HORIZONTAL_TOLERANCE = 100;

    // Check if dragged node can be docked at the top
    if (!draggedNode.canDockTop) {
      return {targetNodeId: null, isValidZone: false};
    }

    const draggedPos = {x: draggedX, y: draggedY};

    for (const node of nodes) {
      if (node.id === draggedNode.id) continue;

      // Find the last node in this chain
      let lastInChain: Node | Omit<Node, 'x' | 'y'> = node;
      while (lastInChain.next) {
        lastInChain = lastInChain.next;
      }

      // Check if last node in chain allows docking below it
      if (!lastInChain.canDockBottom) {
        continue; // Skip this node as a dock target
      }

      const nodePos = {x: node.x, y: node.y};
      const lastDims = getNodeDimensions(lastInChain.id);

      // Calculate position of last node in chain
      let chainHeight = 0;
      let current: Node | Omit<Node, 'x' | 'y'> = node;
      while (current !== lastInChain) {
        chainHeight += getNodeDimensions(current.id).height;
        current = current.next!;
      }

      const nodeBottom = nodePos.y + chainHeight + lastDims.height;

      const verticalDist = draggedPos.y - nodeBottom;
      const isBelow = verticalDist >= -10 && verticalDist <= DOCK_DISTANCE;

      const draggedDims = getNodeDimensions(draggedNode.id);
      const nodeDims = getNodeDimensions(node.id);
      const horizontalDist = Math.abs(
        nodePos.x + nodeDims.width / 2 - (draggedPos.x + draggedDims.width / 2),
      );
      const isAligned = horizontalDist <= HORIZONTAL_TOLERANCE;

      if (isBelow && isAligned) {
        // Return the ID of the LAST node in the chain
        return {targetNodeId: lastInChain.id, isValidZone: true};
      }
    }

    return {targetNodeId: null, isValidZone: false};
  }

  function getNodeDimensions(nodeId: string): {width: number; height: number} {
    const nodeElement = document.querySelector(`[data-node="${nodeId}"]`);
    if (nodeElement) {
      const rect = nodeElement.getBoundingClientRect();
      // Divide by zoom to get canvas content space dimensions
      return {
        width: rect.width / canvasState.zoom,
        height: rect.height / canvasState.zoom,
      };
    }
    // Fallback if DOM element not found
    return {width: 180, height: 100};
  }

  function checkNodeOverlap(
    x: number,
    y: number,
    nodeId: string,
    nodes: ReadonlyArray<Node>,
    nodeWidth: number,
    nodeHeight: number,
  ): boolean {
    const padding = 10;

    for (const node of nodes) {
      if (node.id === nodeId) continue; // Don't check against self

      // Get dimensions of the node we're checking against
      const otherDims = getNodeDimensions(node.id);

      // Calculate total height of the other node's chain
      const chain = getChain(node);
      let otherChainHeight = 0;
      chain.forEach((chainNode) => {
        otherChainHeight += getNodeDimensions(chainNode.id).height;
      });

      const overlaps = !(
        x + nodeWidth + padding < node.x ||
        x > node.x + otherDims.width + padding ||
        y + nodeHeight + padding < node.y ||
        y > node.y + otherChainHeight + padding
      );

      if (overlaps) return true;
    }
    return false;
  }

  function findNearestNonOverlappingPosition(
    startX: number,
    startY: number,
    nodeId: string,
    nodes: ReadonlyArray<Node>,
    nodeWidth: number,
    nodeHeight: number,
  ): Position {
    // If no overlap at current position, return it
    if (
      !checkNodeOverlap(startX, startY, nodeId, nodes, nodeWidth, nodeHeight)
    ) {
      return {x: startX, y: startY};
    }

    // Search in a spiral pattern for a non-overlapping position
    const step = 20; // Step size for searching
    const maxRadius = 500; // Maximum search radius

    for (let radius = step; radius <= maxRadius; radius += step) {
      // Try positions in a circle around the original position
      const numSteps = Math.ceil((2 * Math.PI * radius) / step);

      for (let i = 0; i < numSteps; i++) {
        const angle = (2 * Math.PI * i) / numSteps;
        const x = Math.round(startX + radius * Math.cos(angle));
        const y = Math.round(startY + radius * Math.sin(angle));

        if (!checkNodeOverlap(x, y, nodeId, nodes, nodeWidth, nodeHeight)) {
          return {x, y};
        }
      }
    }

    // Fallback: return original position if no free space found
    return {x: startX, y: startY};
  }

  function getNodesBoundingBox(
    nodes: ReadonlyArray<Node>,
    includeChains: boolean,
  ): {minX: number; minY: number; maxX: number; maxY: number} {
    if (nodes.length === 0) {
      return {minX: 0, minY: 0, maxX: 0, maxY: 0};
    }

    let minX = Infinity;
    let minY = Infinity;
    let maxX = -Infinity;
    let maxY = -Infinity;

    nodes.forEach((node) => {
      const dims = getNodeDimensions(node.id);
      minX = Math.min(minX, node.x);
      minY = Math.min(minY, node.y);
      maxX = Math.max(maxX, node.x + dims.width);

      if (includeChains) {
        const chain = getChain(node);
        let chainHeight = 0;
        chain.forEach((chainNode) => {
          const chainDims = getNodeDimensions(chainNode.id);
          chainHeight += chainDims.height;
        });
        maxY = Math.max(maxY, node.y + chainHeight);
      } else {
        maxY = Math.max(maxY, node.y + dims.height);
      }
    });

    return {minX, minY, maxX, maxY};
  }

  // Helper to perform auto-layout
  function autoLayoutGraph(
    nodes: ReadonlyArray<Node>,
    connections: ReadonlyArray<Connection>,
    onNodeMove: ((nodeId: string, x: number, y: number) => void) | undefined,
  ) {
    // Build a map from any node ID (including nodes in chains) to its root node ID
    const nodeIdToRootId = new Map<string, string>();
    nodes.forEach((node) => {
      nodeIdToRootId.set(node.id, node.id);
      const chain = getChain(node);
      chain.slice(1).forEach((chainNode) => {
        nodeIdToRootId.set(chainNode.id, node.id);
      });
    });

    // Find root nodes (nodes with no incoming connections)
    // Count connections to any node in a chain as connections to the root
    const incomingCounts = new Map<string, number>();
    nodes.forEach((node) => incomingCounts.set(node.id, 0));
    connections.forEach((conn) => {
      const rootId = nodeIdToRootId.get(conn.toNode) ?? conn.toNode;
      const currentCount = incomingCounts.get(rootId) ?? 0;
      incomingCounts.set(rootId, currentCount + 1);
    });

    const rootNodes = nodes.filter((node) => incomingCounts.get(node.id) === 0);
    const visited = new Set<string>();
    const layers: string[][] = [];

    // BFS to assign nodes to layers
    const queue: Array<{id: string; layer: number}> = rootNodes.map((n) => ({
      id: n.id,
      layer: 0,
    }));

    while (queue.length > 0) {
      const {id, layer} = queue.shift()!;
      if (visited.has(id)) continue;
      visited.add(id);

      if (layers[layer] === undefined) layers[layer] = [];
      layers[layer].push(id);

      // Add connected nodes to next layer
      // If connection goes to a node in a chain, add the root node
      connections
        .filter((conn) => {
          // Check if this node or any node in its chain is the source
          const node = nodes.find((n) => n.id === id);
          if (!node) return false;
          const chain = getChain(node);
          return chain.some((chainNode) => chainNode.id === conn.fromNode);
        })
        .forEach((conn) => {
          const rootId = nodeIdToRootId.get(conn.toNode) ?? conn.toNode;
          if (!visited.has(rootId)) {
            queue.push({id: rootId, layer: layer + 1});
          }
        });
    }

    // Position nodes using actual DOM dimensions
    const layerSpacing = 50; // Horizontal spacing between layers
    let currentX = 50; // Start position

    layers.forEach((layer) => {
      // Find the widest node in this layer (considering entire chains)
      let maxWidth = 0;
      layer.forEach((nodeId) => {
        const node = nodes.find((n) => n.id === nodeId);
        if (node) {
          // Check width of all nodes in the chain
          const chain = getChain(node);
          chain.forEach((chainNode) => {
            const chainDims = getNodeDimensions(chainNode.id);
            maxWidth = Math.max(maxWidth, chainDims.width);
          });
        }
      });

      // Position each node in this layer
      let currentY = 50;
      layer.forEach((nodeId) => {
        const node = nodes.find((n) => n.id === nodeId);
        if (node && onNodeMove) {
          onNodeMove(node.id, currentX, currentY);

          // Calculate height of entire chain
          const chain = getChain(node);
          let chainHeight = 0;
          chain.forEach((chainNode) => {
            const dims = getNodeDimensions(chainNode.id);
            chainHeight += dims.height;
          });

          currentY += chainHeight + 30;
        }
      });

      // Move to next layer
      currentX += maxWidth + layerSpacing;
    });

    m.redraw();
  }

  function autofit(
    nodes: ReadonlyArray<Node>,
    labels: ReadonlyArray<Label>,
    canvas: HTMLElement,
  ) {
    if (nodes.length === 0 && labels.length === 0) return;

    // Initialize bounding box
    // If we have nodes, start with their bounding box
    // If we only have labels, initialize with Infinity values that will be replaced
    let minX = Infinity;
    let minY = Infinity;
    let maxX = -Infinity;
    let maxY = -Infinity;

    if (nodes.length > 0) {
      const nodesBBox = getNodesBoundingBox(nodes, true);
      minX = nodesBBox.minX;
      minY = nodesBBox.minY;
      maxX = nodesBBox.maxX;
      maxY = nodesBBox.maxY;
    }

    // Include labels in bounding box calculation
    labels.forEach((label) => {
      minX = Math.min(minX, label.x);
      minY = Math.min(minY, label.y);
      maxX = Math.max(maxX, label.x + label.width);
      maxY = Math.max(maxY, label.y + TYPICAL_LABEL_HEIGHT);
    });

    // Calculate bounding box dimensions
    const boundingWidth = maxX - minX;
    const boundingHeight = maxY - minY;

    // Get canvas dimensions
    const canvasRect = canvas.getBoundingClientRect();

    // Calculate zoom to fit with buffer (10% padding)
    const bufferFactor = 0.9; // Use 90% of viewport to leave 10% buffer
    const zoomX = (canvasRect.width * bufferFactor) / boundingWidth;
    const zoomY = (canvasRect.height * bufferFactor) / boundingHeight;
    const newZoom = Math.max(0.1, Math.min(5.0, Math.min(zoomX, zoomY)));

    // Calculate the scaled bounding box dimensions
    const scaledWidth = boundingWidth * newZoom;
    const scaledHeight = boundingHeight * newZoom;

    // Calculate pan offset to center the bounding box with equal padding on all sides
    const paddingX = (canvasRect.width - scaledWidth) / 2;
    const paddingY = (canvasRect.height - scaledHeight) / 2;

    canvasState.zoom = newZoom;
    canvasState.panOffset = {
      x: paddingX - minX * newZoom,
      y: paddingY - minY * newZoom,
    };

    m.redraw();
  }

  const handleWheel = (e: WheelEvent) => {
    if (!canvasElement) return;
    e.preventDefault();

    // Zoom with Ctrl+wheel, pan without Ctrl
    if (e.ctrlKey || e.metaKey) {
      // Zoom around mouse position
      const canvas = canvasElement;
      const canvasRect = canvas.getBoundingClientRect();
      const mouseX = e.clientX - canvasRect.left;
      const mouseY = e.clientY - canvasRect.top;

      // Calculate zoom delta (negative deltaY = zoom in)
      const zoomDelta = -e.deltaY * 0.003;
      const newZoom = Math.max(
        0.1,
        Math.min(5.0, canvasState.zoom * (1 + zoomDelta)),
      );

      // Calculate the point in canvas space (before zoom)
      const canvasX = (mouseX - canvasState.panOffset.x) / canvasState.zoom;
      const canvasY = (mouseY - canvasState.panOffset.y) / canvasState.zoom;

      // Update zoom
      canvasState.zoom = newZoom;

      // Adjust pan to keep the same point under the mouse
      canvasState.panOffset = {
        x: mouseX - canvasX * newZoom,
        y: mouseY - canvasY * newZoom,
      };
    } else {
      // Pan the canvas based on wheel delta
      canvasState.panOffset = {
        x: canvasState.panOffset.x - e.deltaX,
        y: canvasState.panOffset.y - e.deltaY,
      };
    }

    m.redraw();
  };

  // Helper function to render a single node
  function renderNode(
    node: Node | Omit<Node, 'x' | 'y'>,
    vnode: m.Vnode<NodeGraphAttrs>,
    options: {
      isDockedChild: boolean;
      hasDockedChild: boolean;
      isDockTarget: boolean;
      rootNode?: Node;
      multiselect: boolean;
      contextMenuOnHover: boolean;
    },
  ): m.Vnode {
    const {
      id,
      inputs = [],
      outputs = [],
      titleBar,
      content,
      hue,
      accentBar,
      contextMenuItems,
      invalid,
    } = node;
    const {
      isDockedChild,
      hasDockedChild,
      isDockTarget,
      rootNode,
      multiselect,
      contextMenuOnHover,
    } = options;
    const {connections = [], onConnect, nodes = []} = vnode.attrs;

    // Separate ports by direction
    const topInputs = inputs.filter((p) => p.direction === 'top');
    const leftInputs = inputs.filter((p) => p.direction === 'left');
    const bottomOutputs = outputs.filter((p) => p.direction === 'bottom');
    const rightOutputs = outputs.filter((p) => p.direction === 'right');

    const classes = classNames(
      canvasState.selectedNodes.has(id) && 'pf-selected',
      isDockedChild && 'pf-docked-child',
      hasDockedChild && 'pf-has-docked-child',
      isDockTarget && 'pf-dock-target',
      accentBar && 'pf-node--has-accent-bar',
      invalid && 'pf-invalid',
    );

    // Helper to render a port
    const renderPort = (
      port: NodePort,
      portIndex: number,
      portType: 'input' | 'output',
      forceConnected?: boolean,
    ) => {
      const portId = `${portType}-${portIndex}`;
      const cssClass = classNames(
        portType === 'input' ? 'pf-input' : 'pf-output',
        `pf-port-${port.direction}`,
        (forceConnected ||
          isPortConnected(id, portType, portIndex, connections)) &&
          'pf-connected',
        canvasState.connecting &&
          canvasState.connecting.nodeId === id &&
          canvasState.connecting.portIndex === portIndex &&
          canvasState.connecting.type === portType &&
          'pf-active',
        port.contextMenuItems !== undefined && 'pf-port--with-context-menu',
      );

      const portElement = m('.pf-port', {
        'data-port': portId,
        'className': cssClass,
        'onpointerdown': (e: PointerEvent) => {
          e.stopPropagation();
          if (portType === 'input') {
            // Input port - check for existing connection
            const existingConnIdx = connections.findIndex(
              (conn) => conn.toNode === id && conn.toPort === portIndex,
            );
            if (existingConnIdx !== -1) {
              const existingConn = connections[existingConnIdx];
              const {onConnectionRemove} = vnode.attrs;
              if (onConnectionRemove !== undefined) {
                onConnectionRemove(existingConnIdx);
              }
              const outputPos = getPortPosition(
                existingConn.fromNode,
                'output',
                existingConn.fromPort,
              );
              canvasState.connecting = {
                nodeId: existingConn.fromNode,
                portIndex: existingConn.fromPort,
                type: 'output',
                portType: getPortType(
                  existingConn.fromNode,
                  'output',
                  existingConn.fromPort,
                  nodes,
                ),
                x: 0,
                y: 0,
                transformedX: outputPos.x,
                transformedY: outputPos.y,
              };
              m.redraw();
            }
          } else {
            // Output port - start connection
            const portPos = getPortPosition(id, portType, portIndex);
            canvasState.connecting = {
              nodeId: id,
              portIndex,
              type: portType,
              portType: port.direction,
              x: 0,
              y: 0,
              transformedX: portPos.x,
              transformedY: portPos.y,
            };
          }
        },
        'onpointerup': (e: PointerEvent) => {
          e.stopPropagation();
          if (portType === 'input') {
            if (
              canvasState.connecting &&
              canvasState.connecting.type === 'output'
            ) {
              // Input port receiving connection
              const existingConnIdx = connections.findIndex(
                (conn) => conn.toNode === id && conn.toPort === portIndex,
              );
              if (existingConnIdx !== -1) {
                const {onConnectionRemove} = vnode.attrs;
                if (onConnectionRemove !== undefined) {
                  onConnectionRemove(existingConnIdx);
                }
              }
              const connection = {
                fromNode: canvasState.connecting.nodeId,
                fromPort: canvasState.connecting.portIndex,
                toNode: id,
                toPort: portIndex,
              };
              if (onConnect !== undefined) {
                onConnect(connection);
              }
              canvasState.connecting = null;
            }
          } else if (portType === 'output') {
            // Clear connecting state if releasing on output port without completing connection
            canvasState.connecting = null;
          }
        },
      });

      // Wrap with PopupMenu if contextMenuItems exist
      if (port.contextMenuItems !== undefined) {
        return m(PopupMenu, {trigger: portElement}, port.contextMenuItems);
      }
      return portElement;
    };

    const style = hue !== undefined ? {'--pf-node-hue': `${hue}`} : undefined;

    return m(
      '.pf-node',
      {
        'key': id,
        'data-node': id,
        'class': classes,
        'style': {
          ...style,
        },
        'onpointerdown': (e: PointerEvent) => {
          if ((e.target as HTMLElement).closest('.pf-port')) {
            return;
          }
          e.stopPropagation();

          // Handle multi-selection with Shift or Cmd/Ctrl (only if multiselect is enabled)
          if (multiselect && (e.shiftKey || e.metaKey || e.ctrlKey)) {
            // Toggle selection
            if (canvasState.selectedNodes.has(id)) {
              const {onNodeRemoveFromSelection} = vnode.attrs;
              if (onNodeRemoveFromSelection !== undefined) {
                onNodeRemoveFromSelection(id);
              }
            } else {
              const {onNodeAddToSelection} = vnode.attrs;
              if (onNodeAddToSelection !== undefined) {
                onNodeAddToSelection(id);
              }
            }

            // Focus the canvas element to ensure keyboard events (like Delete) are captured
            if (canvasElement) {
              canvasElement.focus();
            }

            return;
          }

          // Check if this is a chained node (not root)
          if (isDockedChild && rootNode) {
            // Don't undock immediately - wait for drag threshold
            // Calculate current render position
            let yOffset = rootNode.y;
            const chainArr = getChain(rootNode);
            for (const cn of chainArr) {
              if (cn.id === id) break;
              yOffset += getNodeDimensions(cn.id).height;
            }

            // Find parent node in chain
            let parentId = rootNode.id;
            let curr = rootNode.next;
            while (curr && curr.id !== id) {
              parentId = curr.id;
              curr = curr.next;
            }

            // Store undock candidate - will undock if dragged beyond threshold
            canvasState.undockCandidate = {
              nodeId: id,
              parentId: parentId,
              startX: e.clientX,
              startY: e.clientY,
              renderY: yOffset,
            };
          }

          canvasState.draggedNode = id;

          // Store initial drag position for batching
          // Check if node has x,y properties (root nodes) vs docked children (no x,y)
          if ('x' in node && 'y' in node) {
            dragStartPosition = {nodeId: id, x: node.x, y: node.y};
            currentDragPosition = {x: node.x, y: node.y};
          }

          const {onNodeSelect} = vnode.attrs;
          if (onNodeSelect !== undefined) {
            onNodeSelect(id);
          }

          // Focus the canvas element to ensure keyboard events (like Delete) are captured
          if (canvasElement) {
            canvasElement.focus();
          }

          const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
          canvasState.dragOffset = {
            x: e.clientX - rect.left,
            y: e.clientY - rect.top,
          };
        },
      },
      [
        // Render node title if it exists
        titleBar !== undefined &&
          m('.pf-node-header', [
            m('.pf-node-title', titleBar.title),
            contextMenuItems !== undefined &&
              m(
                PopupMenu,
                {
                  trigger: m(Button, {
                    rounded: true,
                    icon: Icons.ContextMenuAlt,
                    className: contextMenuOnHover ? 'pf-show-on-hover' : '',
                  }),
                },
                contextMenuItems,
              ),
          ]),

        // Context menu button for nodes without titlebar
        titleBar === undefined &&
          contextMenuItems !== undefined &&
          m(
            '.pf-node-context-menu',
            {className: contextMenuOnHover ? 'pf-show-on-hover' : ''},
            m(
              PopupMenu,
              {
                trigger: m(Button, {
                  rounded: true,
                  icon: Icons.ContextMenuAlt,
                }),
              },
              contextMenuItems,
            ),
          ),

        // Top input ports (if not docked child)
        topInputs.map((port) => {
          const portIndex = inputs.indexOf(port);
          return renderPort(port, portIndex, 'input');
        }),

        m('.pf-node-body', [
          content !== undefined &&
            m(
              '.pf-node-content',
              {
                onkeydown: (e: KeyboardEvent) => {
                  e.stopPropagation();
                },
              },
              content,
            ),

          // Left input ports
          leftInputs.map((port) => {
            const portIndex = inputs.indexOf(port);
            return m(
              '.pf-port-row.pf-port-input',
              {
                'data-port': `input-${portIndex}`,
              },
              [renderPort(port, portIndex, 'input'), port.content],
            );
          }),

          // Right output ports
          rightOutputs.map((port) => {
            const portIndex = outputs.indexOf(port);
            return m(
              '.pf-port-row.pf-port-output',
              {
                'data-port': `output-${portIndex}`,
              },
              [port.content, renderPort(port, portIndex, 'output')],
            );
          }),
        ]),

        // Bottom output ports (if no docked child below)
        bottomOutputs.map((port) => {
          const portIndex = outputs.indexOf(port);
          return renderPort(port, portIndex, 'output');
        }),
      ],
    );
  }

  function renderLabel(label: Label, vnode: m.Vnode<NodeGraphAttrs>): m.Vnode {
    const {id, x, y, width, content, selectable = false} = label;
    const isDragging = canvasState.draggedLabel === id;
    const isSelected = canvasState.selectedNodes.has(id);

    // Use temporary position/width during drag if available
    const tempPos = canvasState.tempLabelPositions.get(id);
    const tempWidth = canvasState.tempLabelWidths.get(id);
    const renderX = tempPos?.x ?? x;
    const renderY = tempPos?.y ?? y;
    const renderWidth = tempWidth ?? width;

    return m(
      '.pf-label',
      {
        'key': `label-${id}`,
        'data-label': id,
        'className': classNames(
          isDragging && 'pf-dragging',
          isSelected && 'pf-selected',
        ),
        'style': {
          left: `${renderX}px`,
          top: `${renderY}px`,
          width: `${renderWidth}px`,
        },
        'onpointerdown': (e: PointerEvent) => {
          const target = e.target as HTMLElement;

          // Check if clicking on the resize handle or delete button
          if (
            target.closest('.pf-label-resize-handle') ||
            target.closest('.pf-label-delete-button')
          ) {
            e.stopPropagation();
            return;
          }

          // Check if clicking on a textarea that is being edited (not readonly)
          // Allow normal text selection behavior in edit mode
          if (target instanceof HTMLTextAreaElement && !target.readOnly) {
            // Don't start dragging, allow text selection
            return;
          }

          const {multiselect = true} = vnode.attrs;

          // Handle multi-selection with Shift or Cmd/Ctrl (only if multiselect is enabled)
          if (multiselect && (e.shiftKey || e.metaKey || e.ctrlKey)) {
            // Toggle selection
            if (isSelected) {
              const {onNodeRemoveFromSelection} = vnode.attrs;
              if (onNodeRemoveFromSelection !== undefined) {
                onNodeRemoveFromSelection(id);
              }
            } else {
              const {onNodeAddToSelection} = vnode.attrs;
              if (onNodeAddToSelection !== undefined) {
                onNodeAddToSelection(id);
              }
            }

            // Focus the canvas element to ensure keyboard events (like Delete) are captured
            if (canvasElement) {
              canvasElement.focus();
            }

            e.stopPropagation();
            return;
          }

          // Start dragging the label
          canvasState.draggedLabel = id;
          canvasState.dragOffset = {
            x: (canvasState.mousePos.transformedX ?? 0) - x,
            y: (canvasState.mousePos.transformedY ?? 0) - y,
          };

          // Select the label if selectable (replace current selection)
          if (selectable) {
            const {onNodeSelect} = vnode.attrs;
            if (onNodeSelect !== undefined) {
              onNodeSelect(id);
            }
          }

          // Focus the canvas element to ensure keyboard events (like Delete) are captured
          if (canvasElement) {
            canvasElement.focus();
          }

          e.stopPropagation();
        },
      },
      [
        // Render the content (or placeholder if not provided)
        m(
          '.pf-label-content',
          content ?? m('.pf-label-placeholder', 'Empty label'),
        ),
        // Resize handle (always rendered)
        m('.pf-label-resize-handle', {
          onpointerdown: (e: PointerEvent) => {
            // Start resizing
            canvasState.resizingLabel = id;
            canvasState.resizeStartWidth = width;
            canvasState.resizeStartX = canvasState.mousePos.transformedX ?? 0;
            e.stopPropagation();
          },
        }),
        // Delete button (always rendered, visible on hover/selection)
        m(
          '.pf-label-delete-button',
          {
            onclick: (e: PointerEvent) => {
              const {onLabelRemove} = vnode.attrs;
              if (onLabelRemove !== undefined) {
                onLabelRemove(id);
              }
              e.stopPropagation();
            },
          },
          m(Icon, {icon: 'close'}),
        ),
      ],
    );
  }

  return {
    oncreate: (vnode: m.VnodeDOM<NodeGraphAttrs>) => {
      latestVnode = vnode;
      canvasElement = vnode.dom as HTMLElement;
      document.addEventListener('pointermove', handleMouseMove);
      document.addEventListener('pointerup', handleMouseUp);
      canvasElement.addEventListener('wheel', handleWheel, {passive: false});

      const {connections, nodes, onConnectionRemove, onReady} = vnode.attrs;

      // Render connections after DOM is ready
      const svg = vnode.dom.querySelector('svg');
      if (svg) {
        renderConnections(
          svg as SVGElement,
          connections,
          nodes,
          onConnectionRemove,
        );
      }

      // Create auto-layout function that uses actual DOM dimensions
      autoLayoutApi = () => {
        const {nodes = [], connections = [], onNodeMove} = vnode.attrs;
        autoLayoutGraph(nodes, connections, onNodeMove);
      };

      // Create recenter function that brings all nodes into view
      recenterApi = () => {
        if (latestVnode === null || canvasElement === null) {
          return;
        }
        const {nodes = [], labels = []} = latestVnode.attrs;
        const canvas = canvasElement;
        autofit(nodes, labels, canvas);
      };

      // Find a non-overlapping position for a new node
      findPlacementForNodeApi = (newNode: Omit<Node, 'x' | 'y'>): Position => {
        if (latestVnode === null || canvasElement === null) {
          return {x: 0, y: 0};
        }

        const {nodes = []} = latestVnode.attrs;
        const canvas = canvasElement;

        // Default starting position (center of viewport in canvas space)
        const canvasRect = canvas.getBoundingClientRect();
        const centerX =
          (canvasRect.width / 2 - canvasState.panOffset.x) / canvasState.zoom;
        const centerY =
          (canvasRect.height / 2 - canvasState.panOffset.y) / canvasState.zoom;

        // Create a temporary node with coordinates to render and measure
        const tempNode: Node = {
          ...newNode,
          x: centerX,
          y: centerY,
        };

        // Create temporary DOM element to measure size
        const tempContainer = document.createElement('div');
        tempContainer.style.position = 'absolute';
        tempContainer.style.left = '-9999px';
        tempContainer.style.visibility = 'hidden';
        canvas.appendChild(tempContainer);

        // Render the node into the temporary container
        m.render(
          tempContainer,
          m(
            '.pf-node',
            {
              'data-node': tempNode.id,
              'style': {
                ...(tempNode.hue !== undefined
                  ? {'--pf-node-hue': `${tempNode.hue}`}
                  : {}),
              },
            },
            [
              tempNode.titleBar &&
                m('.pf-node-header', [
                  m('.pf-node-title', tempNode.titleBar.title),
                ]),
              m('.pf-node-body', [
                tempNode.content !== undefined &&
                  m('.pf-node-content', tempNode.content),
                tempNode.inputs
                  ?.filter((p) => p.direction === 'left')
                  .map((port) =>
                    m('.pf-port-row.pf-port-input', [
                      m('.pf-port'),
                      port.content,
                    ]),
                  ),
                tempNode.outputs
                  ?.filter((p) => p.direction === 'right')
                  .map((port) =>
                    m('.pf-port-row.pf-port-output', [
                      port.content,
                      m('.pf-port'),
                    ]),
                  ),
              ]),
            ],
          ),
        );

        // Get dimensions from the rendered element
        const dims = getNodeDimensions(tempNode.id);

        // Calculate chain height
        const chain = getChain(tempNode);
        let chainHeight = 0;
        chain.forEach((chainNode) => {
          const chainDims = getNodeDimensions(chainNode.id);
          chainHeight += chainDims.height;
        });

        // Clean up temporary element
        canvas.removeChild(tempContainer);

        // Find non-overlapping position starting from center
        const finalPos = findNearestNonOverlappingPosition(
          centerX - dims.width / 2,
          centerY - dims.height / 2,
          tempNode.id,
          nodes,
          dims.width,
          chainHeight,
        );

        return finalPos;
      };

      // Provide API to parent
      if (
        onReady !== undefined &&
        autoLayoutApi !== null &&
        recenterApi !== null &&
        findPlacementForNodeApi !== null
      ) {
        onReady({
          autoLayout: autoLayoutApi,
          recenter: recenterApi,
          findPlacementForNode: findPlacementForNodeApi,
        });
      }
    },

    onupdate: (vnode: m.VnodeDOM<NodeGraphAttrs>) => {
      latestVnode = vnode;
      const {
        connections = [],
        nodes = [],
        onConnectionRemove,
        onReady,
      } = vnode.attrs;

      // Re-render connections when component updates
      const svg = vnode.dom.querySelector('svg');
      if (svg) {
        renderConnections(
          svg as SVGElement,
          connections,
          nodes,
          onConnectionRemove,
        );
      }

      // Call onReady after every render cycle so parent can perform
      // post-render actions like recentering
      if (
        onReady !== undefined &&
        autoLayoutApi !== null &&
        recenterApi !== null &&
        findPlacementForNodeApi !== null
      ) {
        onReady({
          autoLayout: autoLayoutApi,
          recenter: recenterApi,
          findPlacementForNode: findPlacementForNodeApi,
        });
      }
    },

    onremove: (vnode: m.VnodeDOM<NodeGraphAttrs>) => {
      document.removeEventListener('pointermove', handleMouseMove);
      document.removeEventListener('pointerup', handleMouseUp);
      (vnode.dom as HTMLElement).removeEventListener('wheel', handleWheel);
    },

    view: (vnode: m.Vnode<NodeGraphAttrs>) => {
      latestVnode = vnode;
      const {
        nodes,
        selectedNodeIds = new Set<string>(),
        hideControls = false,
        multiselect = true,
        contextMenuOnHover = false,
        fillHeight,
      } = vnode.attrs;

      // Sync internal state with prop
      canvasState.selectedNodes = selectedNodeIds;

      const className = classNames(
        fillHeight && 'pf-canvas--fill-height',
        canvasState.connecting && 'pf-connecting',
        canvasState.connecting &&
          `connecting-from-${canvasState.connecting.type}`,
        canvasState.isPanning && 'pf-panning',
      );

      return m(
        '.pf-canvas',
        {
          className,
          tabindex: 0, // Make div focusable to capture keyboard events
          oncontextmenu: (e: Event) => {
            e.preventDefault(); // Disable default context menu
          },
          onpointerdown: (e: PointerEvent) => {
            const target = e.target as HTMLElement;
            if (
              target.classList.contains('pf-canvas') ||
              target.tagName === 'svg'
            ) {
              // Start box selection with Shift (only if multiselect is enabled)
              if (multiselect && e.shiftKey) {
                const transformedX = canvasState.mousePos.transformedX ?? 0;
                const transformedY = canvasState.mousePos.transformedY ?? 0;
                canvasState.selectionRect = {
                  startX: transformedX,
                  startY: transformedY,
                  currentX: transformedX,
                  currentY: transformedY,
                };
                return;
              }

              // Start panning and store position to detect click vs drag
              canvasState.isPanning = true;
              canvasState.panStart = {x: e.clientX, y: e.clientY};
              canvasState.canvasMouseDownPos = {x: e.clientX, y: e.clientY};
            }
          },
          onclick: (e: PointerEvent) => {
            const target = e.target as HTMLElement;
            // Clear selection on canvas click (only if mouse didn't move significantly)
            if (
              target.classList.contains('pf-canvas') ||
              target.tagName === 'svg'
            ) {
              const dx = Math.abs(e.clientX - canvasState.canvasMouseDownPos.x);
              const dy = Math.abs(e.clientY - canvasState.canvasMouseDownPos.y);
              const threshold = 3; // Pixels of movement tolerance

              // Only clear if it was a click (not a drag)
              if (dx <= threshold && dy <= threshold) {
                const {onSelectionClear} = vnode.attrs;
                if (onSelectionClear !== undefined) {
                  onSelectionClear();
                }
              }
            }
          },
          onkeydown: (e: KeyboardEvent) => {
            if (e.key === 'Escape') {
              // Deselect all nodes and labels
              const hasSelection = canvasState.selectedNodes.size > 0;
              if (hasSelection) {
                const {onSelectionClear} = vnode.attrs;
                if (onSelectionClear !== undefined) {
                  onSelectionClear();
                }
              }
            } else if (e.key === 'Delete' || e.key === 'Backspace') {
              const {onNodeRemove, onLabelRemove, labels = []} = vnode.attrs;

              if (canvasState.selectedNodes.size > 0) {
                // Flatten all nodes including docked nodes (via 'next' property)
                const allNodeIds = new Set<string>();
                const queue: Array<Node | DockedNode> = [...nodes];
                while (queue.length > 0) {
                  const node = queue.shift();
                  if (node) {
                    allNodeIds.add(node.id);
                    // Traverse docked children via 'next' property
                    if (node.next) {
                      queue.push(node.next);
                    }
                  }
                }

                const labelIds = new Set(labels.map((l) => l.id));

                // Delete selected nodes and labels
                canvasState.selectedNodes.forEach((id) => {
                  if (allNodeIds.has(id) && onNodeRemove !== undefined) {
                    onNodeRemove(id);
                  } else if (labelIds.has(id) && onLabelRemove !== undefined) {
                    onLabelRemove(id);
                  }
                });
              }
            }
          },
          style: {
            backgroundSize: `${20 * canvasState.zoom}px ${20 * canvasState.zoom}px`,
            backgroundPosition: `${canvasState.panOffset.x}px ${canvasState.panOffset.y}px`,
            ...vnode.attrs.style,
          },
        },
        [
          // Control buttons (can be hidden via hideControls prop)
          !hideControls &&
            m('.pf-nodegraph-controls', [
              vnode.attrs.toolbarItems,
              m(Button, {
                label: 'Auto Layout',
                icon: 'account_tree',
                variant: ButtonVariant.Filled,
                onclick: () => {
                  const {
                    nodes = [],
                    connections = [],
                    onNodeMove,
                  } = vnode.attrs;
                  autoLayoutGraph(nodes, connections, onNodeMove);
                },
              }),
              m(Button, {
                label: 'Fit to Screen',
                icon: 'center_focus_strong',
                variant: ButtonVariant.Filled,
                onclick: (e: PointerEvent) => {
                  const {nodes = [], labels = []} = vnode.attrs;
                  const canvas = (e.currentTarget as HTMLElement).closest(
                    '.pf-canvas',
                  );
                  if (canvas) {
                    autofit(nodes, labels, canvas as HTMLElement);
                  }
                },
              }),
            ]),

          // Container for nodes and SVG that gets transformed
          m(
            '.pf-canvas-content',
            {
              style: `transform: translate(${canvasState.panOffset.x}px, ${canvasState.panOffset.y}px) scale(${canvasState.zoom}); transform-origin: 0 0;`,
            },
            [
              // SVG container for connections (rendered imperatively in oncreate/onupdate)
              m('svg'),

              // Selection rectangle overlay
              canvasState.selectionRect &&
                m('.pf-selection-rect', {
                  style: {
                    left: `${Math.min(canvasState.selectionRect.startX, canvasState.selectionRect.currentX)}px`,
                    top: `${Math.min(canvasState.selectionRect.startY, canvasState.selectionRect.currentY)}px`,
                    width: `${Math.abs(canvasState.selectionRect.currentX - canvasState.selectionRect.startX)}px`,
                    height: `${Math.abs(canvasState.selectionRect.currentY - canvasState.selectionRect.startY)}px`,
                  },
                }),

              // Render all nodes - wrap dock chains in flex container
              nodes
                .map((node: Node) => {
                  const {id} = node;

                  // Check if this is the root of a dock chain
                  const chain = getChain(node);
                  const isChainRoot = chain.length > 1;

                  // Check if we have a temp position for this node (during drag)
                  const tempPos = canvasState.tempNodePositions.get(id);
                  const renderPos = tempPos || {x: node.x, y: node.y};

                  // If this is a chain root, wrap all chain nodes in flex container
                  // Always wrap in a chain root container for consistency

                  if (isChainRoot) {
                    return m(
                      '.pf-node-wrapper',
                      {
                        key: `chain-${id}`,
                        style: `left: ${renderPos.x}px; top: ${renderPos.y}px;`,
                        className: classNames(
                          canvasState.draggedNode === id &&
                            'pf-node-wrapper--dragging',
                        ),
                      },
                      chain.map((chainNode) => {
                        const cIsDockedChild = 'x' in chainNode === false;
                        const cHasDockedChild = chainNode.next !== undefined;
                        const cIsDockTarget =
                          canvasState.dockTarget === chainNode.id &&
                          canvasState.isDockZone;

                        return renderNode(chainNode, vnode, {
                          isDockedChild: cIsDockedChild,
                          hasDockedChild: cHasDockedChild,
                          isDockTarget: cIsDockTarget,
                          rootNode: node,
                          multiselect,
                          contextMenuOnHover,
                        });
                      }),
                    );
                  } else {
                    // Render standalone node (not part of a chain)
                    const isDockTarget =
                      canvasState.dockTarget === id && canvasState.isDockZone;

                    return m(
                      '.pf-node-wrapper',
                      {
                        key: `chain-${id}`,
                        style: `left: ${renderPos.x}px; top: ${renderPos.y}px;`,
                        className: classNames(
                          canvasState.draggedNode === id &&
                            'pf-node-wrapper--dragging',
                        ),
                      },
                      renderNode(node, vnode, {
                        isDockedChild: false,
                        hasDockedChild: false,
                        isDockTarget,
                        rootNode: undefined,
                        multiselect,
                        contextMenuOnHover,
                      }),
                    );
                  }
                })
                .filter((vnode) => vnode !== null),

              // Render all labels
              (vnode.attrs.labels ?? []).map((label: Label) => {
                return renderLabel(label, vnode);
              }),
            ],
          ),
        ],
      );
    },
  };
}
