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
import {PopupMenu} from './menu';
import {Button} from './button';

// ========================================
// TYPE DEFINITIONS
// ========================================
interface Position {
  x: number;
  y: number;
  transformedX?: number;
  transformedY?: number;
}

interface Connection {
  fromNode: string;
  fromPort: number;
  toNode: string;
  toPort: number;
}

interface Node {
  id: string;
  title: string;
  x: number;
  y: number;
  inputs?: string[];
  outputs?: string[];
  content?: m.Children; // Optional custom content to render in node body
  contextMenu?: m.Children; // Optional context menu items
}

interface ConnectingState {
  nodeId: string;
  portIndex: number;
  type: 'input' | 'output';
  x: number;
  y: number;
  transformedX: number;
  transformedY: number;
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
}

interface NodeGraphApi {
  autoLayout: () => void;
  recenter: () => void;
}

interface NodeCanvasAttrs {
  readonly nodes?: Node[];
  readonly connections?: Connection[];
  readonly onConnect?: (connection: Connection) => void;
  readonly onNodeDrag?: (nodeId: string, x: number, y: number) => void;
  readonly onConnectionRemove?: (index: number) => void;
  readonly onReady?: (api: NodeGraphApi) => void;
}

interface NodeGraphDOM extends Element {
  _handleMouseMove?: (e: MouseEvent) => void;
  _handleMouseUp?: () => void;
  _handleWheel?: (e: WheelEvent) => void;
}

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
};

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

function getPortPosition(
  nodeId: string,
  portType: 'input' | 'output',
  portIndex: number,
): Position {
  const portElement = document.querySelector(
    `[data-node="${nodeId}"] [data-port="${portType}-${portIndex}"] .pf-port`,
  );

  if (portElement) {
    const nodeElement = portElement.closest('.pf-node') as HTMLElement | null;
    if (nodeElement !== null) {
      const nodeLeft = parseFloat(nodeElement.style.left) || 0;
      const nodeTop = parseFloat(nodeElement.style.top) || 0;

      // Get port's position relative to the node (in screen space, then unscale)
      const portRect = portElement.getBoundingClientRect();
      const nodeRect = nodeElement.getBoundingClientRect();

      // Calculate offset in screen space, then divide by zoom to get canvas content space
      const portX =
        (portRect.left - nodeRect.left + portRect.width / 2) / canvasState.zoom;
      const portY =
        (portRect.top - nodeRect.top + portRect.height / 2) / canvasState.zoom;

      return {
        x: nodeLeft + portX,
        y: nodeTop + portY,
      };
    }
  }

  return {x: 0, y: 0};
}

function renderConnections(
  svg: SVGElement,
  connections: Connection[],
  onConnectionRemove?: (index: number) => void,
) {
  // Clear existing paths
  svg.innerHTML = '';

  connections.forEach((conn, idx) => {
    const from = getPortPosition(conn.fromNode, 'output', conn.fromPort);
    const to = getPortPosition(conn.toNode, 'input', conn.toPort);

    if (from.x !== 0 || from.y !== 0) {
      const path = document.createElementNS(
        'http://www.w3.org/2000/svg',
        'path',
      );
      path.setAttribute('class', 'pf-connection');
      path.setAttribute('d', createCurve(from.x, from.y, to.x, to.y));
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
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('class', 'pf-temp-connection');

    // Convert screen coordinates to canvas content coordinates
    const fromX = canvasState.connecting.transformedX;
    const fromY = canvasState.connecting.transformedY;
    const toX = canvasState.mousePos.transformedX ?? 0;
    const toY = canvasState.mousePos.transformedY ?? 0;

    path.setAttribute('d', createCurve(fromX, fromY, toX, toY));
    svg.appendChild(path);
  }
}

function createCurve(x1: number, y1: number, x2: number, y2: number): string {
  const dx = x2 - x1;
  const dy = y2 - y1;
  const distance = Math.sqrt(dx * dx + dy * dy);

  // Create more dramatic curves based on distance and direction
  const offsetX = Math.max(Math.abs(dx) * 0.6, distance * 0.4);
  const offsetY = dy * 0.3;

  // Control points for smooth S-curve
  const cx1 = x1 + offsetX;
  const cy1 = y1 + offsetY;
  const cx2 = x2 - offsetX;
  const cy2 = y2 - offsetY;

  return `M ${x1} ${y1} C ${cx1} ${cy1}, ${cx2} ${cy2}, ${x2} ${y2}`;
}

function getNodeDimensions(nodeId: string): {width: number; height: number} {
  const nodeElement = document.querySelector(`[data-node="${nodeId}"]`);
  if (nodeElement) {
    const rect = nodeElement.getBoundingClientRect();
    return {width: rect.width, height: rect.height};
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

    const overlaps = !(
      x + nodeWidth + padding < node.x ||
      x > node.x + otherDims.width + padding ||
      y + nodeHeight + padding < node.y ||
      y > node.y + otherDims.height + padding
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
  if (!checkNodeOverlap(startX, startY, nodeId, nodes, nodeWidth, nodeHeight)) {
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
export const NodeGraph: m.Component<NodeCanvasAttrs> = {
  oncreate: (vnode: m.VnodeDOM<NodeCanvasAttrs>) => {
    const {connections = [], onConnectionRemove, onReady} = vnode.attrs;

    // Render connections after DOM is ready
    const svg = vnode.dom.querySelector('svg');
    if (svg) {
      renderConnections(svg as SVGElement, connections, onConnectionRemove);
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
            const dims = getNodeDimensions(nodeId);
            currentY += dims.height + 30; // Move down for next node (30px vertical spacing)
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

    const handleMouseMove = (e: MouseEvent) => {
      const canvas = vnode.dom as HTMLElement;
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
      } else if (canvasState.draggedNode !== null) {
        // Get canvas container's position
        const canvas = vnode.dom as HTMLElement;
        const canvasRect = canvas.getBoundingClientRect();

        // Calculate new position relative to canvas container (accounting for pan and zoom)
        const newX =
          (e.clientX - canvasRect.left - canvasState.panOffset.x) /
            canvasState.zoom -
          canvasState.dragOffset.x / canvasState.zoom;
        const newY =
          (e.clientY - canvasRect.top - canvasState.panOffset.y) /
            canvasState.zoom -
          canvasState.dragOffset.y / canvasState.zoom;

        const {onNodeDrag} = vnode.attrs;
        if (onNodeDrag !== undefined) {
          onNodeDrag(canvasState.draggedNode, newX, newY);
        }
        m.redraw();
      }

      if (canvasState.connecting) {
        m.redraw();
      }
    };

    const handleMouseUp = () => {
      // Check for collision and adjust position if needed
      if (canvasState.draggedNode !== null) {
        const {nodes = [], onNodeDrag} = vnode.attrs;
        const draggedNode = nodes.find((n) => n.id === canvasState.draggedNode);

        if (draggedNode && onNodeDrag) {
          // Get actual node dimensions from DOM
          const dims = getNodeDimensions(draggedNode.id);

          // Check if node overlaps with any other nodes
          if (
            checkNodeOverlap(
              draggedNode.x,
              draggedNode.y,
              draggedNode.id,
              nodes,
              dims.width,
              dims.height,
            )
          ) {
            // Find nearest non-overlapping position
            const newPos = findNearestNonOverlappingPosition(
              draggedNode.x,
              draggedNode.y,
              draggedNode.id,
              nodes,
              dims.width,
              dims.height,
            );
            // Update to the non-overlapping position
            onNodeDrag(draggedNode.id, newPos.x, newPos.y);
          }
        }
      }

      canvasState.draggedNode = null;
      canvasState.connecting = null;
      canvasState.isPanning = false;
      m.redraw();
    };

    const canvas = vnode.dom as HTMLElement;
    canvas.addEventListener('wheel', handleWheel, {passive: false});
    document.addEventListener('mousemove', handleMouseMove);
    document.addEventListener('mouseup', handleMouseUp);

    // Store handlers on the vnode's dom for cleanup
    const dom = vnode.dom as NodeGraphDOM;
    dom._handleMouseMove = handleMouseMove;
    dom._handleMouseUp = handleMouseUp;
    dom._handleWheel = handleWheel;
  },

  onupdate: (vnode: m.VnodeDOM<NodeCanvasAttrs>) => {
    const {connections = [], onConnectionRemove} = vnode.attrs;

    // Re-render connections when component updates
    const svg = vnode.dom.querySelector('svg');
    if (svg) {
      renderConnections(svg as SVGElement, connections, onConnectionRemove);
    }
  },

  onremove: (vnode: m.VnodeDOM<NodeCanvasAttrs>) => {
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

  view: (vnode: m.Vnode<NodeCanvasAttrs>) => {
    const {nodes = [], connections = [], onConnect} = vnode.attrs;

    return m(
      '.pf-canvas',
      {
        onmousedown: (e: MouseEvent) => {
          const target = e.target as HTMLElement;
          if (
            target.classList.contains('pf-canvas') ||
            target.tagName === 'svg'
          ) {
            // Start panning if clicking on canvas background or SVG
            canvasState.selectedNode = null;
            canvasState.isPanning = true;
            canvasState.panStart = {x: e.clientX, y: e.clientY};
            e.preventDefault();
          }
        },
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
                    const dims = getNodeDimensions(nodeId);
                    currentY += dims.height + 30; // Move down for next node (30px vertical spacing)
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

                m.redraw();
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

            // Render all nodes
            nodes.map((node: Node) => {
              const {id, title, x, y, inputs = [], outputs = []} = node;

              const classes =
                canvasState.selectedNode === id ? 'pf-selected' : '';

              return m(
                '.pf-node',
                {
                  'key': id,
                  'data-node': id,
                  'class': classes,
                  'style': `left: ${x}px; top: ${y}px; z-index: ${canvasState.draggedNode === id ? 1000 : 10}`,
                  'onmousedown': (e: MouseEvent) => {
                    if ((e.target as HTMLElement).closest('.pf-port')) return;
                    e.stopPropagation();
                    canvasState.draggedNode = id;
                    canvasState.selectedNode = id;
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
                  m('.pf-node-header', [
                    m('span.pf-node-title', title),
                    node.contextMenu !== undefined &&
                      m(
                        PopupMenu,
                        {
                          trigger: m(Button, {
                            icon: 'more_vert',
                            rounded: true,
                          }),
                        },
                        node.contextMenu,
                      ),
                  ]),
                  m('.pf-node-body', [
                    // Render custom content if provided
                    node.content !== undefined &&
                      m('.pf-node-content', node.content),

                    // Render inputs
                    inputs.map((input: string, i: number) =>
                      m(
                        '.pf-port-row.pf-port-input',
                        {
                          'key': `input-${i}`,
                          'data-port': `input-${i}`,
                        },
                        [
                          m('.pf-port.pf-input', {
                            class: isPortConnected(id, 'input', i, connections)
                              ? 'pf-connected'
                              : '',
                            onmousedown: (e: MouseEvent) => {
                              e.stopPropagation();

                              // Check if this input is already connected
                              const existingConnIdx = connections.findIndex(
                                (conn) =>
                                  conn.toNode === id && conn.toPort === i,
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
                                  x: 0, // Not used anymore
                                  y: 0, // Not used anymore
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
                                    conn.toNode === id && conn.toPort === i,
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
                                  toPort: i,
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

                    // Render outputs
                    outputs.map((output: string, i: number) =>
                      m(
                        '.pf-port-row.pf-port-output',
                        {
                          'key': `output-${i}`,
                          'data-port': `output-${i}`,
                        },
                        [
                          m('span', output),
                          m('.pf-port.pf-output', {
                            class: [
                              isPortConnected(id, 'output', i, connections)
                                ? 'pf-connected'
                                : '',
                              canvasState.connecting &&
                              canvasState.connecting.nodeId === id &&
                              canvasState.connecting.portIndex === i
                                ? 'pf-active'
                                : '',
                            ]
                              .filter(Boolean)
                              .join(' '),
                            onmousedown: (e: MouseEvent) => {
                              e.stopPropagation();
                              const portPos = getPortPosition(id, 'output', i);
                              canvasState.connecting = {
                                nodeId: id,
                                portIndex: i,
                                type: 'output',
                                x: 0, // Not used anymore
                                y: 0, // Not used anymore
                                transformedX: portPos.x,
                                transformedY: portPos.y,
                              };
                            },
                          }),
                        ],
                      ),
                    ),
                  ]),
                ],
              );
            }),
          ],
        ),
      ],
    );
  },
};
