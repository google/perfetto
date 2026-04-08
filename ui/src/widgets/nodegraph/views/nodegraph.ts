// Copyright (C) 2026 The Android Open Source Project
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
import {assertIsInstance} from '../../../base/assert';
import {Point2D} from '../../../base/geom';
import {MithrilEvent} from '../../../base/mithril_utils';
import {DockedNode, Node, NodeGraphAttrs} from '../model';
import {NGCardHeader, NGNode, NGPort, NGCard, NGCardBody} from './node';
import {NGToolbar} from './toolbar';
import {captureDrag} from '../../../base/dom_utils';
import {DisposableStack} from '../../../base/disposable_stack';
import {shortUuid} from '../../../base/uuid';
import {arrowheadMarker, connectionPath} from '../svg';
import type {PortDirection} from '../svg';
import {PopupMenu} from '../../../widgets/menu';
import {PopupPosition} from '../../../widgets/popup';
import {start} from 'repl';

const WHEEL_ZOOM_SCALING_FACTOR = 0.006;
const MIN_ZOOM = 0.01;
const MAX_ZOOM = 3.0;
const GRID_SIZE = 24;

function snapToGrid(v: number): number {
  return Math.round(v / GRID_SIZE) * GRID_SIZE;
}

function portDirFromEl(el: Element): PortDirection {
  if (el.classList.contains('pf-port-north')) return 'top';
  if (el.classList.contains('pf-port-south')) return 'bottom';
  if (el.classList.contains('pf-port-east')) return 'right';
  if (el.classList.contains('pf-input')) return 'left';
  return 'right';
}

function oppositeDir(dir: PortDirection): PortDirection {
  switch (dir) {
    case 'top':
      return 'bottom';
    case 'bottom':
      return 'top';
    case 'left':
      return 'right';
    case 'right':
      return 'left';
  }
}

function dotBackground(zoom: number, offset: {x: number; y: number}) {
  let gridSize = GRID_SIZE;
  while (gridSize * zoom < 10) gridSize *= 2;
  const size = gridSize * zoom;
  return {
    size,
    posX: offset.x * zoom - size / 2,
    posY: offset.y * zoom - size / 2,
  };
}

export interface NodeGraphViewport {
  readonly offset: Point2D;
  readonly zoom: number;
}

export class NodeGraph implements m.Component<NodeGraphAttrs> {
  // Unique ID for this instance's SVG marker, to avoid conflicts when multiple
  // NodeGraph instances exist in the document (e.g. different tabs).
  private readonly markerId = `pf-ng-arrow-${shortUuid()}`;

  // The current viewport transform. Updated during zoom and pan operations, and
  // applied to the canvas via CSS transform.
  private viewport: NodeGraphViewport = {offset: {x: 0, y: 0}, zoom: 1.0};

  // Cached references to important immutable DOM elements. Set in oncreate,
  // used in event handlers.
  private rootEl!: HTMLElement;
  private workspaceEl!: HTMLElement;
  private svgEl!: SVGSVGElement;

  // Non-null while the user is dragging a wire from an output port.
  private wireDrag?: {
    fromPortId: string;
    toPoint: Point2D;
    toDir: PortDirection;
  };

  // Port ID of the output port whose context menu is currently open.
  private openPortMenuId?: string;

  private getNodePosition(nodeEl: HTMLElement): Point2D {
    const rect = nodeEl.getBoundingClientRect();
    const canvasRect = this.rootEl.getBoundingClientRect();
    return this.getWorkspacePosition({
      x: rect.left - canvasRect.left,
      y: rect.top - canvasRect.top,
    });
  }

  private getWorkspacePosition(pos: Point2D): Point2D {
    const {offset, zoom} = this.viewport;
    return {
      x: pos.x / zoom + offset.x,
      y: pos.y / zoom + offset.y,
    };
  }

  private createGhostNode(nodeEl: HTMLElement) {
    const ghost = nodeEl.cloneNode(true) as HTMLElement;
    ghost.removeAttribute('data-node-id'); // avoid confusion with the real node
    ghost.style.position = 'absolute';
    ghost.style.zIndex = '-1'; // behind the node, but above the connections
    ghost.style.pointerEvents = 'none';
    ghost.style.filter = 'brightness(0) opacity(0.5)';
    this.workspaceEl.appendChild(ghost);

    const workspaceEl = this.workspaceEl; // capture for closure

    return {
      element: ghost,
      moveTo(p: Point2D) {
        ghost.style.left = `${p.x}px`;
        ghost.style.top = `${p.y}px`;
      },
      dockToNode(nodeEl: HTMLElement) {
        const card = assertIsInstance(
          nodeEl.querySelector('.pf-ng__card'),
          HTMLElement,
        );
        card.after(ghost);
        ghost.style.removeProperty('left');
        ghost.style.removeProperty('top');
        ghost.style.removeProperty('position');
      },
      undock() {
        ghost.style.position = 'absolute';
        workspaceEl.appendChild(ghost);
      },
      [Symbol.dispose]() {
        ghost.remove();
      },
    };
  }

  private moveNodeToWorkspace(nodeEl: HTMLElement) {
    const previousStyle = nodeEl.getAttribute('style') ?? '';
    const previousParent = nodeEl.parentElement;

    this.workspaceEl.appendChild(nodeEl);
    nodeEl.style.position = 'absolute';

    return {
      moveTo(p: Point2D) {
        nodeEl.style.left = `${p.x}px`;
        nodeEl.style.top = `${p.y}px`;
      },
      [Symbol.dispose]: () => {
        // Move the element back to its original parent and reset styles
        previousParent?.appendChild(nodeEl);
        nodeEl.setAttribute('style', previousStyle);
      },
    };
  }

  view({attrs}: m.Vnode<NodeGraphAttrs>) {
    const {
      onViewportMove,
      onNodeMove,
      onNodeDock,
      onSelect,
      onSelectionAdd,
      onSelectionRemove,
      onSelectionClear,
      onConnect,
      nodes = [],
      selectedNodeIds,
      toolbarItems,
      style,
      className,
    } = attrs;

    // parentId is set for docked nodes; absent for root nodes.
    const renderNode = (node: DockedNode, parentId?: string): m.Children => {
      const isDocked = parentId !== undefined;
      const rootNode = !isDocked ? (node as Node) : undefined;

      return m(
        NGNode,
        {
          key: rootNode ? node.id : undefined,
          id: node.id,
          position: rootNode ? {x: rootNode.x, y: rootNode.y} : undefined,
          // Recursively render the whole tree
          nextNode: node.next && renderNode(node.next, node.id),
          onpointerdown: async (e: MithrilEvent<PointerEvent>) => {
            // Let interactive elements (inputs, buttons, selects, etc.) handle
            // their own events without triggering a node drag.
            const target = e.target as HTMLElement;
            if (target.closest('input, button, select, textarea, a')) return;

            // Stop this pointer event from hitting the canvas
            e.stopPropagation();

            // Don't redraw just yet...
            e.redraw = false;

            // Secure the node element
            const nodeEl = assertIsInstance(e.currentTarget, HTMLElement);

            // Wait for this to turn into a proper drag
            const drag = await captureDrag({el: this.rootEl, e, deadzone: 5});

            if (drag) {
              // Work out where in the workspace the node is right now
              const startPosition = this.getNodePosition(nodeEl);

              using tempNode = this.moveNodeToWorkspace(nodeEl);
              tempNode.moveTo(startPosition);

              using ghost = this.createGhostNode(nodeEl);
              ghost.moveTo(startPosition);

              let currentNodePos = startPosition;
              let dockTargetId: string | undefined = undefined;

              using edgePan = this.startEdgePanning((dx, dy) => {
                currentNodePos = {
                  x: currentNodePos.x + dx,
                  y: currentNodePos.y + dy,
                };
                tempNode.moveTo(currentNodePos);
                this.updateConnections(attrs);
              });

              for await (const mv of drag) {
                const {zoom} = this.viewport;
                edgePan.updatePointer(mv.client);
                currentNodePos = {
                  x: currentNodePos.x + mv.delta.x / zoom,
                  y: currentNodePos.y + mv.delta.y / zoom,
                };

                tempNode.moveTo(currentNodePos);

                dockTargetId = node.canDockTop
                  ? this.findDockTarget(nodeEl)
                  : undefined;

                console.log('Dock target:', dockTargetId);

                if (dockTargetId) {
                  const targetEl = this.getNodeElement(dockTargetId);
                  ghost.dockToNode(targetEl);
                } else {
                  ghost.undock();
                  ghost.moveTo({
                    x: snapToGrid(currentNodePos.x),
                    y: snapToGrid(currentNodePos.y),
                  });
                }
                this.updateConnections(attrs);
              }

              if (dockTargetId) {
                if (dockTargetId !== parentId) {
                  onNodeDock?.(node.id, dockTargetId);
                }
              } else {
                onNodeMove?.(
                  node.id,
                  snapToGrid(currentNodePos.x),
                  snapToGrid(currentNodePos.y),
                );
              }

              m.redraw();
            } else {
              // Failed drag - treat as a click and select the node
              if (e.ctrlKey || e.metaKey) {
                if (selectedNodeIds?.has(node.id)) {
                  onSelectionRemove?.(node.id);
                } else {
                  onSelectionAdd?.(node.id);
                }
              } else {
                // No key held - just select this node and deselect everything else
                onSelect?.([node.id]);
              }
              m.redraw();
            }
          },
        },
        m(
          NGCard,
          {
            hue: node.hue,
            accent: node.accentBar,
            selected: selectedNodeIds?.has(node.id),
            className: node.className,
          },
          [
            node.titleBar &&
              m(NGCardHeader, {
                title: node.titleBar.title,
                icon: node.titleBar.icon,
              }),
            node.inputs?.map((input) =>
              m(NGPort, {
                id: input.id,
                direction: input.direction,
                portType: 'input',
                label: input.label,
                connected:
                  attrs.connections?.some((c) => c.toPort === input.id) ??
                  false,
                onpointerdown: (e: PointerEvent) => {
                  // Stop propagation to prevent the node from being dragged.
                  e.stopPropagation();
                },
                onclick: () => {
                  const connIdx =
                    attrs.connections?.findIndex(
                      (c) => c.toPort === input.id,
                    ) ?? -1;
                  if (connIdx !== -1) {
                    attrs.onDisconnect?.(connIdx);
                  }
                },
              }),
            ),
            m(NGCardBody, node.content),
            node.outputs?.map((output) => {
              const portEl = m(NGPort, {
                id: output.id,
                direction: output.direction,
                portType: 'output',
                label: output.label,
                connected:
                  attrs.connections?.some((c) => c.fromPort === output.id) ??
                  false,
                onpointerdown: async (e: PointerEvent) => {
                  e.stopPropagation();
                  const drag = await captureDrag({
                    el: e.currentTarget as HTMLElement,
                    e,
                  });

                  if (!drag) {
                    // Click (no drag): open context menu if available
                    if (output.contextMenuItems != null) {
                      this.openPortMenuId = output.id;
                      m.redraw();
                    }
                    return;
                  }

                  let clientX = e.clientX;
                  let clientY = e.clientY;

                  const toCanvasPoint = (): Point2D => {
                    const {zoom, offset} = this.viewport;
                    const rect = this.rootEl.getBoundingClientRect();
                    return {
                      x: (clientX - rect.left) / zoom + offset.x,
                      y: (clientY - rect.top) / zoom + offset.y,
                    };
                  };

                  const nodeDirToPortDir: Record<string, PortDirection> = {
                    north: 'top',
                    south: 'bottom',
                    east: 'right',
                    west: 'left',
                  };
                  const freeToDir = oppositeDir(
                    nodeDirToPortDir[output.direction] ?? 'right',
                  );
                  const makeWireDrag = (snap: typeof snapTarget) => ({
                    fromPortId: output.id,
                    toPoint: snap
                      ? this.portCenterToCanvas(snap.el)
                      : toCanvasPoint(),
                    toDir: snap ? portDirFromEl(snap.el) : freeToDir,
                  });

                  this.wireDrag = makeWireDrag(undefined);
                  this.rootEl.classList.add('pf-ng--wire-dragging');
                  this.updateConnections(attrs);

                  let snapTarget: {el: HTMLElement; portId: string} | undefined;

                  using edgePan = this.startEdgePanning(() => {
                    this.wireDrag = makeWireDrag(snapTarget);
                    this.updateConnections(attrs);
                  });

                  const processMove = (client: {x: number; y: number}) => {
                    clientX = client.x;
                    clientY = client.y;
                    edgePan.updatePointer(client);
                    const prevSnap = snapTarget;
                    snapTarget = this.findWireSnapTarget(
                      node.id,
                      clientX,
                      clientY,
                    );
                    if (prevSnap?.el !== snapTarget?.el) {
                      prevSnap?.el.classList.remove('pf-wire-snap');
                      snapTarget?.el.classList.add('pf-wire-snap');
                    }
                    this.wireDrag = makeWireDrag(snapTarget);
                    this.updateConnections(attrs);
                  };

                  for await (const mv of drag) {
                    processMove(mv.client);
                  }
                  snapTarget?.el.classList.remove('pf-wire-snap');
                  this.rootEl.classList.remove('pf-ng--wire-dragging');

                  if (snapTarget !== undefined) {
                    onConnect?.({
                      fromPort: output.id,
                      toPort: snapTarget.portId,
                    });
                  } else {
                    // Fallback: check if the pointer is directly over an input port.
                    const target = document.elementFromPoint(clientX, clientY);
                    const inputPortEl =
                      target?.closest('.pf-ng__port-dot.pf-input') ?? null;
                    if (inputPortEl) {
                      const nodeEl = inputPortEl.closest(
                        '[data-node-id]',
                      ) as HTMLElement | null;
                      const toNodeId = nodeEl?.getAttribute('data-node-id');
                      const portId = inputPortEl.getAttribute('data-port-id');
                      if (portId && toNodeId && toNodeId !== node.id) {
                        onConnect?.({
                          fromPort: output.id,
                          toPort: portId,
                        });
                      }
                    }
                  }

                  this.wireDrag = undefined;
                  this.updateConnections(attrs);
                  m.redraw();
                },
              });
              if (output.contextMenuItems == null) return portEl;
              return m(
                PopupMenu,
                {
                  trigger: portEl,
                  position: PopupPosition.Bottom,
                  isOpen: this.openPortMenuId === output.id,
                  onChange: (open) => {
                    this.openPortMenuId = open ? output.id : undefined;
                    m.redraw();
                  },
                },
                output.contextMenuItems,
              );
            }),
          ],
        ),
      );
    };

    return m(
      '.pf-ng',
      {
        style,
        className,
        onwheel: (e: MithrilEvent<WheelEvent>) => {
          e.preventDefault();
          if (e.ctrlKey || e.metaKey) {
            const newZoom =
              this.viewport.zoom *
              Math.exp(-e.deltaY * WHEEL_ZOOM_SCALING_FACTOR);
            this.zoomViewport(newZoom, {x: e.clientX, y: e.clientY});
          } else {
            this.panViewport(e.deltaX, e.deltaY);
          }
          onViewportMove?.(this.viewport);
        },
        oncontextmenu: (e: MouseEvent) => {
          e.preventDefault();
        },
        onpointerdown: async (e: MithrilEvent<PointerEvent>) => {
          e.redraw = false;
          const nodegraph = this.rootEl;
          const isBoxSelect = e.shiftKey;
          const drag = await captureDrag({
            el: nodegraph,
            e,
            deadzone: 2,
          });
          if (drag) {
            if (isBoxSelect) {
              const boxEl = document.createElement('div');
              boxEl.style.cssText =
                'position:absolute;pointer-events:none;' +
                'border:1px dashed var(--pf-color-primary);' +
                'background:color-mix(in srgb,var(--pf-color-primary) 10%,transparent);' +
                'z-index:10;';
              this.rootEl.appendChild(boxEl);

              const updateBox = (currentClient: {x: number; y: number}) => {
                const ngRect = this.rootEl.getBoundingClientRect();
                const x1 = Math.min(e.clientX, currentClient.x) - ngRect.left;
                const y1 = Math.min(e.clientY, currentClient.y) - ngRect.top;
                const x2 = Math.max(e.clientX, currentClient.x) - ngRect.left;
                const y2 = Math.max(e.clientY, currentClient.y) - ngRect.top;
                Object.assign(boxEl.style, {
                  left: `${x1}px`,
                  top: `${y1}px`,
                  width: `${x2 - x1}px`,
                  height: `${y2 - y1}px`,
                });
              };

              let currentClient = {x: e.clientX, y: e.clientY};
              for await (const mv of drag) {
                currentClient = {x: mv.client.x, y: mv.client.y};
                updateBox(currentClient);
              }

              boxEl.remove();

              const boxLeft = Math.min(e.clientX, currentClient.x);
              const boxTop = Math.min(e.clientY, currentClient.y);
              const boxRight = Math.max(e.clientX, currentClient.x);
              const boxBottom = Math.max(e.clientY, currentClient.y);
              const ids: string[] = [];
              for (const nodeEl of this.rootEl.querySelectorAll(
                '[data-node-id]',
              )) {
                const r = nodeEl.getBoundingClientRect();
                if (
                  r.left < boxRight &&
                  r.right > boxLeft &&
                  r.top < boxBottom &&
                  r.bottom > boxTop
                ) {
                  ids.push(nodeEl.getAttribute('data-node-id')!);
                }
              }
              if (ids.length > 0) {
                onSelect?.(ids);
              }
            } else {
              for await (const mv of drag) {
                this.panViewport(-mv.delta.x, -mv.delta.y);
              }
              onViewportMove?.(this.viewport);
            }
          } else {
            onSelectionClear?.();
          }
          m.redraw();
        },
      },
      m(
        '.pf-ng__workspace',
        nodes.map((n) => renderNode(n)),
        m('svg.pf-ng__connections', {
          style: {
            position: 'absolute',
            left: '0',
            top: '0',
            overflow: 'visible',
            pointerEvents: 'none',
          },
        }),
      ),
      m(NGToolbar, {
        zoom: this.viewport.zoom,
        onZoom: (level) => {
          this.zoomViewport(level);
          onViewportMove?.(this.viewport);
        },
        onFit: () => {
          this.autofit();
          onViewportMove?.(this.viewport);
        },
        extraItems: toolbarItems,
      }),
      m('.pf-ng__trashcan'),
    );
  }

  oncreate({dom, attrs}: m.VnodeDOM<NodeGraphAttrs>) {
    this.rootEl = assertIsInstance(dom, HTMLElement);
    this.workspaceEl = assertIsInstance(
      dom.querySelector('.pf-ng__workspace'),
      HTMLElement,
    );
    this.svgEl = assertIsInstance(
      dom.querySelector('.pf-ng__connections'),
      SVGSVGElement,
    );
    const {initialViewport = {offset: {x: 0, y: 0}, zoom: 1.0}} = attrs;
    this.viewport = {
      zoom: initialViewport.zoom,
      offset: {...initialViewport.offset},
    };
    this.updateViewport();
    this.updateConnections(attrs);
  }

  onupdate({attrs}: m.VnodeDOM<NodeGraphAttrs>) {
    this.updateConnections(attrs);
  }

  private getNodeElement(nodeId: string) {
    return assertIsInstance(
      this.rootEl.querySelector(`[data-node-id="${nodeId}"]`),
      HTMLElement,
    );
  }

  // Returns the ID of the root node whose chain bottom is close enough to
  // snap-dock the dropped node onto. The dragged node must have canDockTop and
  // the candidate must have canDockBottom. Position comparison is done in
  // viewport space using the rendered wrapper's bounding rect.
  // Returns the ID of a node whose bottom edge is close to the top of the
  // dragged node element, with some horizontal overlap.
  private findDockTarget(draggedNodeEl: HTMLElement): string | undefined {
    const THRESHOLD = 30; // viewport px
    const draggedRect = draggedNodeEl.getBoundingClientRect();
    for (const el of this.rootEl.querySelectorAll('[data-node-id]')) {
      const id = el.getAttribute('data-node-id')!;
      // Use the node-content bottom so nested docked children don't inflate the rect.
      const body = el.querySelector(
        ':scope > .pf-ng__card',
      ) as HTMLElement | null;
      const rect = (body ?? (el as HTMLElement)).getBoundingClientRect();
      if (
        Math.abs(draggedRect.top - rect.bottom) < THRESHOLD &&
        draggedRect.right > rect.left - THRESHOLD &&
        draggedRect.left < rect.right + THRESHOLD
      ) {
        return id;
      }
    }
    return undefined;
  }

  // Returns the input port element + connection target closest to (clientX,
  // clientY), or undefined if nothing is within the snap threshold.
  private findWireSnapTarget(
    fromNodeId: string,
    clientX: number,
    clientY: number,
  ): {el: HTMLElement; portId: string} | undefined {
    const THRESHOLD = 40; // viewport px
    let best: {el: HTMLElement; portId: string; dist: number} | undefined;

    for (const el of this.rootEl.querySelectorAll(
      '.pf-ng__port-dot.pf-input',
    )) {
      if (el.closest('.pf-hidden')) continue;
      const nodeEl = el.closest('[data-node-id]') as HTMLElement | null;
      const toNodeId = nodeEl?.getAttribute('data-node-id');
      if (!toNodeId || toNodeId === fromNodeId) continue;
      const portId = el.getAttribute('data-port-id');
      if (!portId) continue;
      const rect = el.getBoundingClientRect();
      const dist = Math.hypot(
        clientX - (rect.left + rect.width / 2),
        clientY - (rect.top + rect.height / 2),
      );
      if (dist < THRESHOLD && (!best || dist < best.dist)) {
        best = {el: el as HTMLElement, portId, dist};
      }
    }

    return best ? {el: best.el, portId: best.portId} : undefined;
  }

  // Converts an input port element's center to canvas coordinates.
  private portCenterToCanvas(portEl: HTMLElement): Point2D {
    const rect = portEl.getBoundingClientRect();
    const ngRect = this.rootEl.getBoundingClientRect();
    const {zoom, offset} = this.viewport;
    return {
      x: (rect.left + rect.width / 2 - ngRect.left) / zoom + offset.x,
      y: (rect.top + rect.height / 2 - ngRect.top) / zoom + offset.y,
    };
  }

  private updateViewport() {
    const {offset, zoom} = this.viewport;
    const canvas = this.rootEl;
    this.workspaceEl.style.transform = `scale(${zoom}) translate(${-offset.x}px, ${-offset.y}px)`;
    const {size, posX, posY} = dotBackground(zoom, offset);
    canvas.style.setProperty('--bg-size', `${size}px`);
    canvas.style.setProperty('--bg-pos-x', `${-posX}px`);
    canvas.style.setProperty('--bg-pos-y', `${-posY}px`);
  }

  private zoomViewport(zoom: number, center?: Point2D) {
    const canvas = this.rootEl;
    const rect = canvas.getBoundingClientRect();
    if (!center) {
      center = {x: rect.left + rect.width / 2, y: rect.top + rect.height / 2};
    }
    const {offset, zoom: currentZoom} = this.viewport;
    const newZoom = Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, zoom));
    const mouseX = center.x - rect.left;
    const mouseY = center.y - rect.top;
    const canvasX = mouseX / currentZoom + offset.x;
    const canvasY = mouseY / currentZoom + offset.y;
    this.viewport = {
      zoom: newZoom,
      offset: {
        x: canvasX - mouseX / newZoom,
        y: canvasY - mouseY / newZoom,
      },
    };
    this.updateViewport();
  }

  private panViewport(dx: number, dy: number) {
    const {offset, zoom} = this.viewport;
    this.viewport = {
      ...this.viewport,
      offset: {x: offset.x + dx / zoom, y: offset.y + dy / zoom},
    };
    this.updateViewport();
  }

  // Starts a RAF loop that pans the viewport when the pointer is near an edge.
  // `getClientPos` returns the current pointer position in client coordinates.
  // `onPan` (optional) is called with canvas-space deltas so the caller can
  // update any accumulated canvas-space state (e.g. node position).
  // Returns a stop function; call it when the drag ends.
  private startEdgePanning(
    onPan?: (canvasDx: number, canvasDy: number) => void,
  ) {
    const ZONE = 30; // viewport px from edge where panning begins
    const MAX_SPEED = 10; // viewport px per frame at the very edge

    const edgeForce = (v: number, lo: number, hi: number): number => {
      if (v < lo + ZONE) return (-(lo + ZONE - v) / ZONE) * MAX_SPEED;
      if (v > hi - ZONE) return ((v - (hi - ZONE)) / ZONE) * MAX_SPEED;
      return 0;
    };

    let rafId: number | undefined;
    let latestPointerPos: Point2D | undefined;

    const frame = () => {
      const {x: cx, y: cy} = latestPointerPos!;
      const rect = this.rootEl.getBoundingClientRect();
      const vpDx = edgeForce(cx, rect.left, rect.right);
      const vpDy = edgeForce(cy, rect.top, rect.bottom);
      if (vpDx !== 0 || vpDy !== 0) {
        const {zoom} = this.viewport;
        this.panViewport(vpDx, vpDy);
        onPan?.(vpDx / zoom, vpDy / zoom);
      }
      rafId = requestAnimationFrame(frame);
    };

    return {
      updatePointer: (pos: Point2D) => {
        latestPointerPos = pos;
        if (rafId === undefined) rafId = requestAnimationFrame(frame);
      },
      [Symbol.dispose]: () => {
        if (rafId !== undefined) cancelAnimationFrame(rafId);
      },
    };
  }

  private updateConnections(attrs: NodeGraphAttrs) {
    const {connections = []} = attrs;
    const ngRect = this.rootEl.getBoundingClientRect();
    const {zoom, offset} = this.viewport;

    const getPortInfo = (portId: string) => {
      const candidates = this.rootEl.querySelectorAll(
        `.pf-ng__port-dot[data-port-id="${portId}"]`,
      );
      const el = Array.from(candidates).find(
        (e) => e.closest('.pf-hidden') === null,
      ) as HTMLElement | undefined;
      if (!el) return undefined;
      const rect = el.getBoundingClientRect();

      const dir = portDirFromEl(el);

      return {
        x: (rect.left + rect.width / 2 - ngRect.left) / zoom + offset.x,
        y: (rect.top + rect.height / 2 - ngRect.top) / zoom + offset.y,
        dir,
      };
    };

    const paths = connections.flatMap((conn) => {
      const from = getPortInfo(conn.fromPort);
      const to = getPortInfo(conn.toPort);
      if (!from || !to) return [];
      return [connectionPath(from, to, this.markerId, from.dir, to.dir)];
    });

    if (this.wireDrag) {
      const {fromPortId, toPoint, toDir} = this.wireDrag;
      const from = getPortInfo(fromPortId);
      if (from) {
        paths.push(
          connectionPath(from, toPoint, this.markerId, from.dir, toDir, {
            'stroke-dasharray': '6 3',
          }),
        );
      }
    }

    m.render(this.svgEl, [m('defs', arrowheadMarker(this.markerId)), ...paths]);
  }

  private autofit() {
    const PADDING = 40; // screen px of breathing room on each side
    const nodeEls = Array.from(
      this.workspaceEl.querySelectorAll(':scope > [data-node-id]'),
    ) as HTMLElement[];

    // If there are no nodes, do nothing.
    if (nodeEls.length === 0) {
      return;
    }

    // Node left/top style is in canvas px; offsetWidth/Height are layout px
    // (unaffected by the workspace CSS transform), so also canvas px.
    let minX = Infinity;
    let minY = Infinity;
    let maxX = -Infinity;
    let maxY = -Infinity;

    for (const el of nodeEls) {
      const x = parseFloat(el.style.left) || 0;
      const y = parseFloat(el.style.top) || 0;
      minX = Math.min(minX, x);
      minY = Math.min(minY, y);
      maxX = Math.max(maxX, x + el.offsetWidth);
      maxY = Math.max(maxY, y + el.offsetHeight);
    }

    const containerW = this.rootEl.offsetWidth;
    const containerH = this.rootEl.offsetHeight;
    const bbW = maxX - minX;
    const bbH = maxY - minY;

    const zoom = Math.max(
      MIN_ZOOM,
      Math.min(
        1.0,
        Math.min(
          (containerW - 2 * PADDING) / bbW,
          (containerH - 2 * PADDING) / bbH,
        ),
      ),
    );

    // Center: canvas midpoint should map to screen midpoint.
    // screen = (canvas - offset) * zoom  →  offset = canvas - screen / zoom
    this.viewport = {
      zoom,
      offset: {
        x: (minX + maxX) / 2 - containerW / (2 * zoom),
        y: (minY + maxY) / 2 - containerH / (2 * zoom),
      },
    };
    this.updateViewport();
  }
}
