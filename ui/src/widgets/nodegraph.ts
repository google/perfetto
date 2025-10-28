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
import {Button} from './button';
import {PopupMenu} from './menu';

// ========================================
// TYPE DEFINITIONS
// ========================================
interface Position {
  x: number;
  y: number;
  transformedX?: number;
  transformedY?: number;
}

export interface Connection {
  fromNode: string;
  fromPort: number;
  toNode: string;
  toPort: number;
}

export interface Node {
  id: string;
  x: number;
  y: number;
  inputs?: string[];
  outputs?: string[];
  content?: m.Children; // Optional custom content to render in node body
  contextMenu?: m.Children; // Optional context menu items
  next?: Omit<Node, 'x' | 'y'>; // Next node in chain (linked list)
  addMenuItems?: m.Children;
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

interface CanvasState {
  draggedNode: string | null;
  dragOffset: Position;
  connecting: ConnectingState | null;
  mousePos: Position;
  selectedNode: string | null;
  panOffset: Position;
  isPanning: boolean;
  panStart: Position;
  zoom: number;
  dockTarget: string | null; // Node being targeted for docking
  isDockZone: boolean; // Whether we're in valid dock position
  undockCandidate: UndockCandidate | null; // Tracks potential undock before threshold
}

export interface NodeGraphApi {
  autoLayout: () => void;
  recenter: () => void;
}

export interface NodeGraphAttrs {
  readonly nodes?: Node[];
  readonly connections?: Connection[];
  readonly onConnect?: (connection: Connection) => void;
  readonly onNodeDrag?: (nodeId: string, x: number, y: number) => void;
  readonly onConnectionRemove?: (index: number) => void;
  readonly onReady?: (api: NodeGraphApi) => void;
  readonly selectedNodeId?: string | null;
  readonly onNodeSelect?: (nodeId: string | null) => void;
  readonly onDock?: (
    parentId: string,
    childNode: Omit<Node, 'x' | 'y'>,
  ) => void;
  readonly onUndock?: (parentId: string) => void;
  readonly onNodeRemove?: (nodeId: string) => void;
}

interface NodeGraphDOM extends Element {
  _handleMouseMove?: (e: MouseEvent) => void;
  _handleMouseUp?: () => void;
  _handleWheel?: (e: WheelEvent) => void;
}

// ========================================
// CONSTANTS
// ========================================
const UNDOCK_THRESHOLD = 5; // Pixels to drag before undocking

// ========================================
// HELPER FUNCTIONS
// ========================================
function isPortConnected(
  nodeId: string,
  portType: 'input' | 'output',
  portIndex: number,
  connections: Connection[],
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
): string {
  const dx = x2 - x1;
  const dy = y2 - y1;
  const distance = Math.sqrt(dx * dx + dy * dy);

  let cx1: number;
  let cy1: number;
  let cx2: number;
  let cy2: number;

  // For top/bottom ports, control points extend vertically
  // For left/right ports, control points extend horizontally
  if (fromPortType === 'bottom' || fromPortType === 'top') {
    // First control point extends vertically
    const verticalOffset = Math.max(Math.abs(dy) * 0.6, distance * 0.4);
    cx1 = x1;
    cy1 = fromPortType === 'bottom' ? y1 + verticalOffset : y1 - verticalOffset;
  } else {
    // First control point extends horizontally for left/right ports
    const horizontalOffset = Math.max(Math.abs(dx) * 0.6, distance * 0.4);
    cx1 = x1 + horizontalOffset;
    cy1 = y1; // Keep Y constant for horizontal extension
  }

  if (toPortType === 'bottom' || toPortType === 'top') {
    // Second control point extends vertically
    const verticalOffset = Math.max(Math.abs(dy) * 0.6, distance * 0.4);
    cx2 = x2;
    cy2 = toPortType === 'bottom' ? y2 + verticalOffset : y2 - verticalOffset;
  } else {
    // Second control point extends horizontally for left/right ports
    const horizontalOffset = Math.max(Math.abs(dx) * 0.6, distance * 0.4);
    cx2 = x2 - horizontalOffset;
    cy2 = y2; // Keep Y constant for horizontal extension
  }

  return `M ${x1} ${y1} C ${cx1} ${cy1}, ${cx2} ${cy2}, ${x2} ${y2}`;
}

// Auto-layout nodes in a hierarchical arrangement based on connections
export function autoLayoutNodes(
  nodes: Node[],
  connections: Connection[],
): void {
  // Find root nodes (nodes with no incoming connections)
  const incomingCounts = new Map<string, number>();
  nodes.forEach((node) => incomingCounts.set(node.id, 0));
  connections.forEach((conn) => {
    const currentCount = incomingCounts.get(conn.toNode) ?? 0;
    incomingCounts.set(conn.toNode, currentCount + 1);
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
    connections
      .filter((conn) => conn.fromNode === id)
      .forEach((conn) => {
        if (!visited.has(conn.toNode)) {
          queue.push({id: conn.toNode, layer: layer + 1});
        }
      });
  }

  // Position nodes
  const layerSpacing = 300;
  const nodeSpacing = 120;

  layers.forEach((layer, layerIndex) => {
    layer.forEach((nodeId, nodeIndex) => {
      const node = nodes.find((n) => n.id === nodeId);
      if (node) {
        node.x = layerIndex * layerSpacing + 50;
        node.y = nodeIndex * nodeSpacing + 50;
      }
    });
  });
}

// ========================================
// CANVAS COMPONENT
// ========================================
export function NodeGraph(): m.Component<NodeGraphAttrs> {
  // ========================================
  // CANVAS STATE (shared across all instances)
  // ========================================
  const canvasState: CanvasState = {
    draggedNode: null,
    dragOffset: {x: 0, y: 0},
    connecting: null,
    mousePos: {x: 0, y: 0},
    selectedNode: null,
    panOffset: {x: 0, y: 0},
    isPanning: false,
    panStart: {x: 0, y: 0},
    zoom: 1.0,
    dockTarget: null,
    isDockZone: false,
    undockCandidate: null,
  };

  // Helper to determine port type based on port index
  function getPortType(
    nodeId: string,
    portType: 'input' | 'output',
    portIndex: number,
    nodes: Node[],
  ): 'top' | 'bottom' | 'left' | 'right' {
    // Search in main nodes array
    let node = nodes.find((n) => n.id === nodeId);

    // If not found, search in the next chains of all nodes
    if (!node) {
      for (const rootNode of nodes) {
        let current = rootNode.next;
        while (current) {
          if (current.id === nodeId) {
            node = current as Node;
            break;
          }
          current = current.next;
        }
        if (node) break;
      }
    }

    if (!node) return portType === 'input' ? 'left' : 'right';

    if (portType === 'input') {
      return portIndex === 0 ? 'top' : 'left';
    } else {
      return portIndex === 0 ? 'bottom' : 'right';
    }
  }

  function renderConnections(
    svg: SVGElement,
    connections: Connection[],
    nodes: Node[],
    onConnectionRemove?: (index: number) => void,
  ) {
    // Clear existing paths
    svg.innerHTML = '';

    // Create arrow marker definition
    const defs = document.createElementNS('http://www.w3.org/2000/svg', 'defs');
    const marker = document.createElementNS(
      'http://www.w3.org/2000/svg',
      'marker',
    );
    marker.setAttribute('id', 'arrowhead');
    marker.setAttribute('markerWidth', '10');
    marker.setAttribute('markerHeight', '6');
    marker.setAttribute('refX', '10');
    marker.setAttribute('refY', '3');
    marker.setAttribute('orient', 'auto');
    marker.setAttribute('markerUnits', 'strokeWidth');

    const polygon = document.createElementNS(
      'http://www.w3.org/2000/svg',
      'polygon',
    );
    polygon.setAttribute('points', '0 0, 6 3, 0 6');
    polygon.setAttribute('fill', 'var(--pf-color-accent)');

    marker.appendChild(polygon);
    defs.appendChild(marker);
    svg.appendChild(defs);

    // Only render explicit connections (not implicit dock connections)
    connections.forEach((conn, idx) => {
      const from = getPortPosition(conn.fromNode, 'output', conn.fromPort);
      const to = getPortPosition(conn.toNode, 'input', conn.toPort);

      if (from.x !== 0 || from.y !== 0) {
        const path = document.createElementNS(
          'http://www.w3.org/2000/svg',
          'path',
        );
        path.setAttribute('class', 'pf-connection');

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

        path.setAttribute(
          'd',
          createCurve(from.x, from.y, to.x, to.y, fromPortType, toPortType),
        );
        path.setAttribute('marker-end', 'url(#arrowhead)');
        path.style.pointerEvents = 'stroke';
        path.style.cursor = 'pointer';

        // Prevent canvas pan from starting when clicking connections
        path.onmousedown = (e) => {
          e.stopPropagation();
          e.preventDefault();
        };

        path.onclick = (e) => {
          e.stopPropagation();
          if (onConnectionRemove !== undefined) {
            onConnectionRemove(idx);
          }
        };
        svg.appendChild(path);
      }
    });

    // Render temp connection if connecting
    if (canvasState.connecting) {
      const path = document.createElementNS(
        'http://www.w3.org/2000/svg',
        'path',
      );
      path.setAttribute('class', 'pf-temp-connection');

      // Convert screen coordinates to canvas content coordinates
      const fromX = canvasState.connecting.transformedX;
      const fromY = canvasState.connecting.transformedY;
      const toX = canvasState.mousePos.transformedX ?? 0;
      const toY = canvasState.mousePos.transformedY ?? 0;

      // For temp connections, use the stored port type
      const fromPortType = canvasState.connecting.portType;
      // The target end defaults to the opposite type for visual feedback
      const toPortType =
        fromPortType === 'top' || fromPortType === 'bottom' ? 'top' : 'left';
      path.setAttribute(
        'd',
        createCurve(fromX, fromY, toX, toY, fromPortType, toPortType),
      );
      svg.appendChild(path);
    }
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
          '.pf-dock-chain',
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
    nodes: Node[],
  ): {targetNodeId: string | null; isValidZone: boolean} {
    const DOCK_DISTANCE = 30;
    const HORIZONTAL_TOLERANCE = 100;

    // Check if dragged node can be docked above others
    // It can dock above if it has inputs (top port exists)
    const draggedCanDockAbove = (draggedNode.inputs?.length ?? 0) > 0;
    if (!draggedCanDockAbove) {
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
      // It can have nodes dock below if it has outputs (bottom port exists)
      const lastCanDockBelow = (lastInChain.outputs?.length ?? 0) > 0;
      if (!lastCanDockBelow) {
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
    nodes: Node[],
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
    nodes: Node[],
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

  return {
    oncreate: (vnode: m.VnodeDOM<NodeGraphAttrs>) => {
      const {
        connections = [],
        nodes = [],
        onConnectionRemove,
        onReady,
      } = vnode.attrs;

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
      const autoLayout = () => {
        const {nodes = [], connections = [], onNodeDrag} = vnode.attrs;

        // Find root nodes (nodes with no incoming connections)
        const incomingCounts = new Map<string, number>();
        nodes.forEach((node) => incomingCounts.set(node.id, 0));
        connections.forEach((conn) => {
          const currentCount = incomingCounts.get(conn.toNode) ?? 0;
          incomingCounts.set(conn.toNode, currentCount + 1);
        });

        const rootNodes = nodes.filter(
          (node) => incomingCounts.get(node.id) === 0,
        );
        const visited = new Set<string>();
        const layers: string[][] = [];

        // BFS to assign nodes to layers
        const queue: Array<{id: string; layer: number}> = rootNodes.map(
          (n) => ({
            id: n.id,
            layer: 0,
          }),
        );

        while (queue.length > 0) {
          const {id, layer} = queue.shift()!;
          if (visited.has(id)) continue;
          visited.add(id);

          if (layers[layer] === undefined) layers[layer] = [];
          layers[layer].push(id);

          // Add connected nodes to next layer
          connections
            .filter((conn) => conn.fromNode === id)
            .forEach((conn) => {
              if (!visited.has(conn.toNode)) {
                queue.push({id: conn.toNode, layer: layer + 1});
              }
            });
        }

        // Position nodes using actual DOM dimensions
        const layerSpacing = 50; // Horizontal spacing between layers
        let currentX = 50; // Start position

        layers.forEach((layer) => {
          // Find the widest node in this layer
          let maxWidth = 0;
          layer.forEach((nodeId) => {
            const dims = getNodeDimensions(nodeId);
            maxWidth = Math.max(maxWidth, dims.width);
          });

          // Position each node in this layer
          let currentY = 50;
          layer.forEach((nodeId) => {
            const node = nodes.find((n) => n.id === nodeId);
            if (node && onNodeDrag) {
              onNodeDrag(node.id, currentX, currentY);

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
      };

      // Create recenter function that brings all nodes into view
      const recenter = () => {
        const {nodes = []} = vnode.attrs;

        if (nodes.length === 0) return;

        // Calculate bounding box of all nodes
        let minX = Infinity;
        let minY = Infinity;
        let maxX = -Infinity;
        let maxY = -Infinity;

        nodes.forEach((node) => {
          const dims = getNodeDimensions(node.id);
          minX = Math.min(minX, node.x);
          minY = Math.min(minY, node.y);
          maxX = Math.max(maxX, node.x + dims.width);
          maxY = Math.max(maxY, node.y + dims.height);
        });

        // Calculate center of bounding box
        const centerX = (minX + maxX) / 2;
        const centerY = (minY + maxY) / 2;

        // Get canvas dimensions
        const canvas = vnode.dom as HTMLElement;
        const canvasRect = canvas.getBoundingClientRect();
        const viewportCenterX = canvasRect.width / 2;
        const viewportCenterY = canvasRect.height / 2;

        // Calculate required pan offset to center the nodes
        canvasState.panOffset = {
          x: viewportCenterX - centerX,
          y: viewportCenterY - centerY,
        };

        m.redraw();
      };

      // Provide API to parent
      if (onReady) {
        onReady({autoLayout, recenter});
      }

      const handleWheel = (e: WheelEvent) => {
        e.preventDefault();

        // Zoom with Ctrl+wheel, pan without Ctrl
        if (e.ctrlKey || e.metaKey) {
          // Zoom around mouse position
          const canvas = vnode.dom as HTMLElement;
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

      const canvas = vnode.dom as HTMLElement;
      canvas.addEventListener('wheel', handleWheel, {passive: false});

      // Store handlers on the vnode's dom for cleanup
      const dom = vnode.dom as NodeGraphDOM;
      dom._handleWheel = handleWheel;
    },

    onupdate: (vnode: m.VnodeDOM<NodeGraphAttrs>) => {
      const {connections = [], nodes = [], onConnectionRemove} = vnode.attrs;

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
    },

    onremove: (vnode: m.VnodeDOM<NodeGraphAttrs>) => {
      // Clean up event listeners
      const dom = vnode.dom as NodeGraphDOM;
      const handleMouseMove = dom._handleMouseMove;
      const handleMouseUp = dom._handleMouseUp;

      if (handleMouseMove !== undefined) {
        document.removeEventListener('mousemove', handleMouseMove);
      }
      if (handleMouseUp !== undefined) {
        document.removeEventListener('mouseup', handleMouseUp);
      }

      const handleWheel = dom._handleWheel;
      if (handleWheel !== undefined) {
        (vnode.dom as HTMLElement).removeEventListener('wheel', handleWheel);
      }
    },

    view: (vnode: m.Vnode<NodeGraphAttrs>) => {
      const {
        nodes = [],
        connections = [],
        onConnect,
        selectedNodeId,
      } = vnode.attrs;

      const handleMouseMove = (e: MouseEvent) => {
        const canvas = (e.target as HTMLElement).closest(
          '.pf-canvas',
        ) as HTMLElement | null;
        if (!canvas) return;
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

        if (canvasState.isPanning) {
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
            // Exceeded threshold - perform undock
            const {onUndock, onNodeDrag} = vnode.attrs;
            if (onUndock && onNodeDrag) {
              onUndock(canvasState.undockCandidate.parentId);
              onNodeDrag(
                canvasState.undockCandidate.nodeId,
                (canvasState.undockCandidate.startX -
                  canvasRect.left -
                  canvasState.panOffset.x) /
                  canvasState.zoom -
                  canvasState.dragOffset.x / canvasState.zoom,
                canvasState.undockCandidate.renderY,
              );
              m.redraw(); // Force update so nodes array regenerates
            }
            canvasState.undockCandidate = null;
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

          // ONLY move the dragged node itself
          // Children follow automatically via render position calculation
          const {onNodeDrag, nodes = []} = vnode.attrs;
          if (onNodeDrag !== undefined) {
            onNodeDrag(canvasState.draggedNode, newX, newY);
          }

          // Check if we're in a dock zone (exclude the parent we just undocked from)
          const draggedNode = nodes.find(
            (n) => n.id === canvasState.draggedNode,
          );
          if (draggedNode) {
            const dockInfo = findDockTarget(draggedNode, newX, newY, nodes);
            canvasState.dockTarget = dockInfo.targetNodeId;
            canvasState.isDockZone = dockInfo.isValidZone;
          }
        }
      };

      const handleMouseUp = () => {
        // Handle docking if in dock zone
        if (
          canvasState.draggedNode &&
          canvasState.isDockZone &&
          canvasState.dockTarget
        ) {
          const {nodes = [], onDock} = vnode.attrs;
          const draggedNode = nodes.find(
            (n) => n.id === canvasState.draggedNode,
          );
          if (onDock && draggedNode) {
            // Create child node without x/y coordinates
            // eslint-disable-next-line @typescript-eslint/no-unused-vars
            const {x, y, ...childNode} = draggedNode;
            onDock(canvasState.dockTarget, childNode);
          }
        }

        // Check for collision (only for non-docked nodes)
        if (canvasState.draggedNode !== null) {
          const {nodes = [], onNodeDrag} = vnode.attrs;
          const draggedNode = nodes.find(
            (n) => n.id === canvasState.draggedNode,
          );

          // Only do overlap checking if NOT being docked
          if (draggedNode && !canvasState.isDockZone && onNodeDrag) {
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
              checkNodeOverlap(
                draggedNode.x,
                draggedNode.y,
                draggedNode.id,
                nodes,
                dims.width,
                chainHeight,
              )
            ) {
              // Find nearest non-overlapping position
              const newPos = findNearestNonOverlappingPosition(
                draggedNode.x,
                draggedNode.y,
                draggedNode.id,
                nodes,
                dims.width,
                chainHeight,
              );
              // Update to the non-overlapping position
              onNodeDrag(draggedNode.id, newPos.x, newPos.y);
            }
          }
        }

        canvasState.draggedNode = null;
        canvasState.connecting = null;
        canvasState.isPanning = false;
        canvasState.dockTarget = null;
        canvasState.isDockZone = false;
        canvasState.undockCandidate = null;
        m.redraw();
      };

      return m(
        '.pf-canvas',
        {
          tabindex: 0, // Make div focusable to capture keyboard events
          onmousedown: (e: MouseEvent) => {
            const target = e.target as HTMLElement;
            if (
              target.classList.contains('pf-canvas') ||
              target.tagName === 'svg'
            ) {
              // Start panning if clicking on canvas background or SVG
              canvasState.selectedNode = null;

              // Call onNodeSelect callback with null to indicate deselection
              const {onNodeSelect} = vnode.attrs;
              if (onNodeSelect !== undefined) {
                onNodeSelect(null);
              }

              canvasState.isPanning = true;
              canvasState.panStart = {x: e.clientX, y: e.clientY};
              e.preventDefault();
            }
          },
          onkeydown: (e: KeyboardEvent) => {
            if (e.key === 'Delete' || e.key === 'Backspace') {
              const {selectedNodeId, onNodeRemove} = vnode.attrs;
              if (selectedNodeId && onNodeRemove) {
                onNodeRemove(selectedNodeId);
                e.preventDefault();
              }
            }
          },
          onmousemove: handleMouseMove,
          onmouseup: handleMouseUp,
          style: `cursor: ${canvasState.isPanning ? 'grabbing' : 'grab'}`,
        },
        [
          // Control buttons
          m('.pf-nodegraph-controls', [
            m(Button, {
              label: 'Auto Layout',
              icon: 'account_tree',
              compact: true,
              onclick: () => {
                const {nodes = [], connections = [], onNodeDrag} = vnode.attrs;

                // Find root nodes (nodes with no incoming connections)
                const incomingCounts = new Map<string, number>();
                nodes.forEach((node) => incomingCounts.set(node.id, 0));
                connections.forEach((conn) => {
                  const currentCount = incomingCounts.get(conn.toNode) ?? 0;
                  incomingCounts.set(conn.toNode, currentCount + 1);
                });

                const rootNodes = nodes.filter(
                  (node) => incomingCounts.get(node.id) === 0,
                );
                const visited = new Set<string>();
                const layers: string[][] = [];

                // BFS to assign nodes to layers
                const queue: Array<{id: string; layer: number}> = rootNodes.map(
                  (n) => ({
                    id: n.id,
                    layer: 0,
                  }),
                );

                while (queue.length > 0) {
                  const {id, layer} = queue.shift()!;
                  if (visited.has(id)) continue;
                  visited.add(id);

                  if (layers[layer] === undefined) layers[layer] = [];
                  layers[layer].push(id);

                  // Add connected nodes to next layer
                  connections
                    .filter((conn) => conn.fromNode === id)
                    .forEach((conn) => {
                      if (!visited.has(conn.toNode)) {
                        queue.push({id: conn.toNode, layer: layer + 1});
                      }
                    });
                }

                // Position nodes using actual DOM dimensions
                const layerSpacing = 50; // Horizontal spacing between layers
                let currentX = 50; // Start position

                layers.forEach((layer) => {
                  // Find the widest node in this layer
                  let maxWidth = 0;
                  layer.forEach((nodeId) => {
                    const dims = getNodeDimensions(nodeId);
                    maxWidth = Math.max(maxWidth, dims.width);
                  });

                  // Position each node in this layer
                  let currentY = 50;
                  layer.forEach((nodeId) => {
                    const node = nodes.find((n) => n.id === nodeId);
                    if (node && onNodeDrag) {
                      onNodeDrag(node.id, currentX, currentY);

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
              },
            }),
            m(Button, {
              label: 'Recenter',
              icon: 'center_focus_strong',
              compact: true,
              onclick: (e: MouseEvent) => {
                const {nodes = []} = vnode.attrs;

                if (nodes.length === 0) return;

                // Calculate bounding box of all nodes
                let minX = Infinity;
                let minY = Infinity;
                let maxX = -Infinity;
                let maxY = -Infinity;

                nodes.forEach((node) => {
                  const dims = getNodeDimensions(node.id);
                  minX = Math.min(minX, node.x);
                  minY = Math.min(minY, node.y);
                  maxX = Math.max(maxX, node.x + dims.width);
                  maxY = Math.max(maxY, node.y + dims.height);
                });

                // Calculate center of bounding box
                const centerX = (minX + maxX) / 2;
                const centerY = (minY + maxY) / 2;

                // Get canvas dimensions
                const canvas = (e.currentTarget as HTMLElement).closest(
                  '.pf-canvas',
                );
                if (canvas) {
                  const canvasRect = canvas.getBoundingClientRect();
                  const viewportCenterX = canvasRect.width / 2;
                  const viewportCenterY = canvasRect.height / 2;

                  // Calculate required pan offset to center the nodes
                  canvasState.panOffset = {
                    x: viewportCenterX - centerX,
                    y: viewportCenterY - centerY,
                  };
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

              // Render all nodes - wrap dock chains in flex container
              nodes
                .map((node: Node) => {
                  const {id, inputs = [], outputs = []} = node;

                  // Check if this is the root of a dock chain
                  const chain = getChain(node);
                  const isChainRoot = chain.length > 1;

                  // Use node's x,y directly (it's a root node)
                  const renderPos = {x: node.x, y: node.y};

                  // If this is a chain root, wrap all chain nodes in flex container
                  if (isChainRoot) {
                    return m(
                      '.pf-dock-chain',
                      {
                        key: `chain-${id}`,
                        style: `left: ${renderPos.x}px; top: ${renderPos.y}px; z-index: ${canvasState.draggedNode === id ? 1000 : 10}`,
                      },
                      chain.map((chainNode) => {
                        const {
                          id: cId,
                          inputs: cInputs = [],
                          outputs: cOutputs = [],
                          addMenuItems,
                        } = chainNode;

                        const cIsDockedChild = 'x' in chainNode === false;
                        const cHasDockedChild = chainNode.next !== undefined;
                        const cIsDockTarget =
                          canvasState.dockTarget === cId &&
                          canvasState.isDockZone;

                        const cClasses = [
                          selectedNodeId === cId ? 'pf-selected' : '',
                          cIsDockedChild ? 'pf-docked-child' : '',
                          cHasDockedChild ? 'pf-has-docked-child' : '',
                          cIsDockTarget ? 'pf-dock-target' : '',
                        ]
                          .filter(Boolean)
                          .join(' ');

                        return m(
                          '.pf-node',
                          {
                            'key': cId,
                            'data-node': cId,
                            'class': cClasses,
                            'onmousedown': (e: MouseEvent) => {
                              if (
                                (e.target as HTMLElement).closest('.pf-port')
                              ) {
                                return;
                              }
                              e.stopPropagation();

                              // Check if this is a chained node (not root)
                              if (!('x' in chainNode)) {
                                // Don't undock immediately - wait for drag threshold
                                // Calculate current render position
                                let yOffset = node.y;
                                const chainArr = getChain(node);
                                for (const cn of chainArr) {
                                  if (cn.id === cId) break;
                                  yOffset += getNodeDimensions(cn.id).height;
                                }

                                // Find parent node in chain
                                let parentId = node.id;
                                let curr = node.next;
                                while (curr && curr.id !== cId) {
                                  parentId = curr.id;
                                  curr = curr.next;
                                }

                                // Store undock candidate - will undock if dragged beyond threshold
                                canvasState.undockCandidate = {
                                  nodeId: cId,
                                  parentId: parentId,
                                  startX: e.clientX,
                                  startY: e.clientY,
                                  renderY: yOffset,
                                };
                              }

                              canvasState.draggedNode = cId;
                              canvasState.selectedNode = cId;

                              const {onNodeSelect} = vnode.attrs;
                              if (onNodeSelect !== undefined) {
                                onNodeSelect(cId);
                              }

                              const rect = (
                                e.currentTarget as HTMLElement
                              ).getBoundingClientRect();
                              canvasState.dragOffset = {
                                x: e.clientX - rect.left,
                                y: e.clientY - rect.top,
                              };
                            },
                          },
                          [
                            // First input on top (if exists and not docked child)
                            cInputs.length > 0 &&
                              !cIsDockedChild &&
                              m('.pf-port.pf-input.pf-port-top', {
                                'data-port': 'input-0',
                                'class': isPortConnected(
                                  cId,
                                  'input',
                                  0,
                                  connections,
                                )
                                  ? 'pf-connected'
                                  : '',
                                'onmousedown': (e: MouseEvent) => {
                                  e.stopPropagation();
                                  const existingConnIdx = connections.findIndex(
                                    (conn) =>
                                      conn.toNode === cId && conn.toPort === 0,
                                  );
                                  if (existingConnIdx !== -1) {
                                    const existingConn =
                                      connections[existingConnIdx];
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
                                },
                                'onmouseup': (e: MouseEvent) => {
                                  e.stopPropagation();
                                  if (
                                    canvasState.connecting &&
                                    canvasState.connecting.type === 'output'
                                  ) {
                                    const existingConnIdx =
                                      connections.findIndex(
                                        (conn) =>
                                          conn.toNode === cId &&
                                          conn.toPort === 0,
                                      );
                                    if (existingConnIdx !== -1) {
                                      const {onConnectionRemove} = vnode.attrs;
                                      if (onConnectionRemove !== undefined) {
                                        onConnectionRemove(existingConnIdx);
                                      }
                                    }
                                    const connection = {
                                      fromNode: canvasState.connecting.nodeId,
                                      fromPort:
                                        canvasState.connecting.portIndex,
                                      toNode: cId,
                                      toPort: 0,
                                    };
                                    if (onConnect !== undefined) {
                                      onConnect(connection);
                                    }
                                    canvasState.connecting = null;
                                  }
                                },
                              }),

                            chainNode.content !== undefined &&
                              m(
                                '.pf-node-content',
                                {
                                  onkeydown: (e: KeyboardEvent) => {
                                    e.stopPropagation();
                                  },
                                },
                                chainNode.content,
                              ),

                            // Remaining inputs on left side (inputs[1+])
                            cInputs.slice(1).map((input: string, i: number) =>
                              m(
                                '.pf-port-row.pf-port-input',
                                {
                                  'data-port': `input-${i + 1}`,
                                },
                                [
                                  m('.pf-port.pf-input', {
                                    class: isPortConnected(
                                      cId,
                                      'input',
                                      i + 1,
                                      connections,
                                    )
                                      ? 'pf-connected'
                                      : '',
                                    onmousedown: (e: MouseEvent) => {
                                      e.stopPropagation();
                                      const existingConnIdx =
                                        connections.findIndex(
                                          (conn) =>
                                            conn.toNode === cId &&
                                            conn.toPort === i + 1,
                                        );
                                      if (existingConnIdx !== -1) {
                                        const existingConn =
                                          connections[existingConnIdx];
                                        const {onConnectionRemove} =
                                          vnode.attrs;
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
                                    },
                                    onmouseup: (e: MouseEvent) => {
                                      e.stopPropagation();
                                      if (
                                        canvasState.connecting &&
                                        canvasState.connecting.type === 'output'
                                      ) {
                                        const existingConnIdx =
                                          connections.findIndex(
                                            (conn) =>
                                              conn.toNode === cId &&
                                              conn.toPort === i + 1,
                                          );
                                        if (existingConnIdx !== -1) {
                                          const {onConnectionRemove} =
                                            vnode.attrs;
                                          if (
                                            onConnectionRemove !== undefined
                                          ) {
                                            onConnectionRemove(existingConnIdx);
                                          }
                                        }
                                        const connection = {
                                          fromNode:
                                            canvasState.connecting.nodeId,
                                          fromPort:
                                            canvasState.connecting.portIndex,
                                          toNode: cId,
                                          toPort: i + 1,
                                        };
                                        if (onConnect !== undefined) {
                                          onConnect(connection);
                                        }
                                        canvasState.connecting = null;
                                      }
                                    },
                                  }),
                                  m('span', input),
                                ],
                              ),
                            ),

                            // Remaining outputs on right side (outputs[1+])
                            cOutputs.slice(1).map((output: string, i: number) =>
                              m(
                                '.pf-port-row.pf-port-output',
                                {
                                  'data-port': `output-${i + 1}`,
                                },
                                [
                                  m('span', output),
                                  m('.pf-port.pf-output', {
                                    class: [
                                      isPortConnected(
                                        cId,
                                        'output',
                                        i + 1,
                                        connections,
                                      )
                                        ? 'pf-connected'
                                        : '',
                                      canvasState.connecting &&
                                      canvasState.connecting.nodeId === cId &&
                                      canvasState.connecting.portIndex === i + 1
                                        ? 'pf-active'
                                        : '',
                                    ]
                                      .filter(Boolean)
                                      .join(' '),
                                    onmousedown: (e: MouseEvent) => {
                                      e.stopPropagation();
                                      const portPos = getPortPosition(
                                        cId,
                                        'output',
                                        i + 1,
                                      );
                                      canvasState.connecting = {
                                        nodeId: cId,
                                        portIndex: i + 1,
                                        type: 'output',
                                        portType: 'right',
                                        x: 0,
                                        y: 0,
                                        transformedX: portPos.x,
                                        transformedY: portPos.y,
                                      };
                                    },
                                  }),
                                ],
                              ),
                            ),

                            // First output on bottom (if exists and no docked child below)
                            cOutputs.length > 0 &&
                              !cHasDockedChild &&
                              m(
                                PopupMenu,
                                {
                                  trigger: m(
                                    '.pf-port.pf-output.pf-port-bottom',
                                    {
                                      'data-port': 'output-0',
                                      'class': [
                                        isPortConnected(
                                          cId,
                                          'output',
                                          0,
                                          connections,
                                        )
                                          ? 'pf-connected'
                                          : '',
                                        canvasState.connecting &&
                                        canvasState.connecting.nodeId === cId &&
                                        canvasState.connecting.portIndex === 0
                                          ? 'pf-active'
                                          : '',
                                      ]
                                        .filter(Boolean)
                                        .join(' '),
                                      'onmousedown': (e: MouseEvent) => {
                                        e.stopPropagation();
                                        const portPos = getPortPosition(
                                          cId,
                                          'output',
                                          0,
                                        );
                                        canvasState.connecting = {
                                          nodeId: cId,
                                          portIndex: 0,
                                          type: 'output',
                                          portType: 'bottom',
                                          x: 0,
                                          y: 0,
                                          transformedX: portPos.x,
                                          transformedY: portPos.y,
                                        };
                                      },
                                    },
                                  ),
                                },
                                addMenuItems,
                              ),
                          ],
                        );
                      }),
                    );
                  }

                  // Render standalone node (not part of a chain)
                  const isDockTarget =
                    canvasState.dockTarget === id && canvasState.isDockZone;

                  const classes = [
                    selectedNodeId === id ? 'pf-selected' : '',
                    isDockTarget ? 'pf-dock-target' : '',
                  ]
                    .filter(Boolean)
                    .join(' ');

                  return m(
                    '.pf-node',
                    {
                      'key': id,
                      'data-node': id,
                      'class': classes,
                      'style': `left: ${renderPos.x}px; top: ${renderPos.y}px; z-index: ${canvasState.draggedNode === id ? 1000 : 10}`,
                      'onmousedown': (e: MouseEvent) => {
                        if ((e.target as HTMLElement).closest('.pf-port')) {
                          return;
                        }
                        e.stopPropagation();

                        // Start dragging
                        canvasState.draggedNode = id;
                        canvasState.selectedNode = id;

                        // Call onNodeSelect callback
                        const {onNodeSelect} = vnode.attrs;
                        if (onNodeSelect !== undefined) {
                          onNodeSelect(id);
                        }

                        const rect = (
                          e.currentTarget as HTMLElement
                        ).getBoundingClientRect();
                        canvasState.dragOffset = {
                          x: e.clientX - rect.left,
                          y: e.clientY - rect.top,
                        };
                      },
                    },
                    [
                      // First input on top (if exists)
                      // Note: standalone nodes are never docked children, so always show if inputs exist
                      inputs.length > 0 &&
                        m('.pf-port.pf-input.pf-port-top', {
                          'data-port': 'input-0',
                          'class': isPortConnected(id, 'input', 0, connections)
                            ? 'pf-connected'
                            : '',
                          'onmousedown': (e: MouseEvent) => {
                            e.stopPropagation();

                            // Check if this input is already connected
                            const existingConnIdx = connections.findIndex(
                              (conn) => conn.toNode === id && conn.toPort === 0,
                            );

                            if (existingConnIdx !== -1) {
                              const existingConn = connections[existingConnIdx];

                              // Remove the existing connection
                              const {onConnectionRemove} = vnode.attrs;
                              if (onConnectionRemove !== undefined) {
                                onConnectionRemove(existingConnIdx);
                              }

                              // Start a new connection from the original output port
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
                          },
                          'onmouseup': (e: MouseEvent) => {
                            e.stopPropagation();
                            if (
                              canvasState.connecting &&
                              canvasState.connecting.type === 'output'
                            ) {
                              // Check if this input already has a connection
                              const existingConnIdx = connections.findIndex(
                                (conn) =>
                                  conn.toNode === id && conn.toPort === 0,
                              );

                              // Remove existing connection if present
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
                                toPort: 0,
                              };

                              // Call onConnect callback if provided
                              if (onConnect !== undefined) {
                                onConnect(connection);
                              }

                              canvasState.connecting = null;
                            }
                          },
                        }),

                      // Render custom content if provided
                      node.content !== undefined &&
                        m(
                          '.pf-node-content',
                          {
                            onkeydown: (e: KeyboardEvent) => {
                              e.stopPropagation();
                            },
                          },
                          node.content,
                        ),

                      // Remaining inputs on left side (inputs[1+])
                      inputs.slice(1).map((input: string, i: number) =>
                        m(
                          '.pf-port-row.pf-port-input',
                          {
                            'data-port': `input-${i + 1}`,
                          },
                          [
                            m('.pf-port.pf-input', {
                              class: isPortConnected(
                                id,
                                'input',
                                i + 1,
                                connections,
                              )
                                ? 'pf-connected'
                                : '',
                              onmousedown: (e: MouseEvent) => {
                                e.stopPropagation();

                                // Check if this input is already connected
                                const existingConnIdx = connections.findIndex(
                                  (conn) =>
                                    conn.toNode === id && conn.toPort === i + 1,
                                );

                                if (existingConnIdx !== -1) {
                                  const existingConn =
                                    connections[existingConnIdx];

                                  // Remove the existing connection
                                  const {onConnectionRemove} = vnode.attrs;
                                  if (onConnectionRemove !== undefined) {
                                    onConnectionRemove(existingConnIdx);
                                  }

                                  // Start a new connection from the original output port
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
                              },
                              onmouseup: (e: MouseEvent) => {
                                e.stopPropagation();
                                if (
                                  canvasState.connecting &&
                                  canvasState.connecting.type === 'output'
                                ) {
                                  // Check if this input already has a connection
                                  const existingConnIdx = connections.findIndex(
                                    (conn) =>
                                      conn.toNode === id &&
                                      conn.toPort === i + 1,
                                  );

                                  // Remove existing connection if present
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
                                    toPort: i + 1,
                                  };

                                  // Call onConnect callback if provided
                                  if (onConnect !== undefined) {
                                    onConnect(connection);
                                  }

                                  canvasState.connecting = null;
                                }
                              },
                            }),
                            m('span', input),
                          ],
                        ),
                      ),

                      // Remaining outputs on right side (outputs[1+])
                      outputs.slice(1).map((output: string, i: number) =>
                        m(
                          '.pf-port-row.pf-port-output',
                          {
                            'data-port': `output-${i + 1}`,
                          },
                          [
                            m('span', output),
                            m('.pf-port.pf-output', {
                              class: [
                                isPortConnected(
                                  id,
                                  'output',
                                  i + 1,
                                  connections,
                                )
                                  ? 'pf-connected'
                                  : '',
                                canvasState.connecting &&
                                canvasState.connecting.nodeId === id &&
                                canvasState.connecting.portIndex === i + 1
                                  ? 'pf-active'
                                  : '',
                              ]
                                .filter(Boolean)
                                .join(' '),
                              onmousedown: (e: MouseEvent) => {
                                e.stopPropagation();
                                const portPos = getPortPosition(
                                  id,
                                  'output',
                                  i + 1,
                                );
                                canvasState.connecting = {
                                  nodeId: id,
                                  portIndex: i + 1,
                                  type: 'output',
                                  portType: 'right',
                                  x: 0,
                                  y: 0,
                                  transformedX: portPos.x,
                                  transformedY: portPos.y,
                                };
                              },
                            }),
                          ],
                        ),
                      ),

                      // First output on bottom (if exists)
                      // Note: standalone nodes never have docked children, so always show if outputs exist
                      outputs.length > 0 &&
                        m(
                          PopupMenu,
                          {
                            trigger: m('.pf-port.pf-output.pf-port-bottom', {
                              'data-port': 'output-0',
                              'class': [
                                isPortConnected(id, 'output', 0, connections)
                                  ? 'pf-connected'
                                  : '',
                                canvasState.connecting &&
                                canvasState.connecting.nodeId === id &&
                                canvasState.connecting.portIndex === 0
                                  ? 'pf-active'
                                  : '',
                              ]
                                .filter(Boolean)
                                .join(' '),
                              'onmousedown': (e: MouseEvent) => {
                                e.stopPropagation();
                                const portPos = getPortPosition(
                                  id,
                                  'output',
                                  0,
                                );
                                canvasState.connecting = {
                                  nodeId: id,
                                  portIndex: 0,
                                  type: 'output',
                                  portType: 'bottom',
                                  x: 0,
                                  y: 0,
                                  transformedX: portPos.x,
                                  transformedY: portPos.y,
                                };
                              },
                            }),
                          },
                          node.addMenuItems,
                        ),
                    ],
                  );
                })
                .filter((vnode) => vnode !== null),
            ],
          ),
        ],
      );
    },
  };
}
