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
import {NodeGraphViewport} from './views/nodegraph';

export interface Connection {
  readonly fromPort: string; // port id
  readonly toPort: string; // port id
}

export interface NodeTitleBar {
  readonly title: m.Children;
  readonly icon?: string;
}

export interface NodePort {
  readonly id: string;
  readonly direction: 'north' | 'south' | 'east' | 'west';
  readonly contextMenuItems?: m.Children;
  readonly label?: m.Children;
}

export type DockedNode = Omit<Node, 'x' | 'y'>;

export interface Node {
  readonly id: string;
  readonly x: number;
  readonly y: number;
  readonly hue: number; // Color of the title / accent bar (0-360)
  readonly accentBar?: boolean; // Optional strip of accent color on the left side (doesn't work well with titleBar)
  readonly titleBar?: NodeTitleBar; // Optional title bar (doesn't work well with accentBar or docking)
  readonly inputs?: ReadonlyArray<NodePort>;
  readonly outputs?: ReadonlyArray<NodePort>;
  readonly content?: m.Children; // Optional custom content to render in node body
  readonly next?: DockedNode; // Next node in chain
  readonly canDockTop?: boolean;
  readonly canDockBottom?: boolean;
  readonly contextMenuItems?: m.Children;
  readonly className?: string; // Extra CSS class(es) on the .pf-node element
}

export interface NodeGraphAPI {
  /**
   * Recenter the canvas to show all nodes. This calculates the bounding box of
   * all nodes and adjusts the pan/zoom to fit them within the viewport. If
   * there are no nodes, this resets pan to (0,0) and zoom to 1.0.
   */
  recenter: () => void;

  /**
   * Calculates a non-overlapping position for a new node being added to the
   * graph. This can be used by parent components to determine where to place
   * new nodes (e.g. from a context menu "Add Node" action) in a way that
   * doesn't overlap existing nodes. The position is returned in canvas
   * coordinates (not transformed by current pan/zoom).
   * @param node - The description of the new node to be added.
   * @returns - An {x, y} position in canvas coordinates.
   */
  findPlacementForNode: (node: Omit<Node, 'x' | 'y'>) => {x: number; y: number};

  /**
   * Pans the canvas by the given delta values. Positive dx pans right, positive
   * dy pans down.
   * @param dx - Delta X to pan (positive pans right, negative pans left).
   * @param dy - Delta Y to pan (positive pans down, negative pans up).
   */
  pan: (dx: number, dy: number) => void;

  /**
   * Zooms the canvas by the given delta factor.
   * @param deltaZoom - The zoom delta (e.g., 0.1 for 10% zoom in, -0.1 for 10% zoom out)
   * @param centerX - X coordinate to zoom around (in viewport space). Defaults to canvas center.
   * @param centerY - Y coordinate to zoom around (in viewport space). Defaults to canvas center.
   */
  zoom: (deltaZoom: number, centerX?: number, centerY?: number) => void;

  /**
   * Reset the canvas zoom level to the default (1.0) retaining the current
   * center point.
   */
  resetZoom: () => void;
}

export interface NodeGraphAttrs {
  readonly className?: string;
  readonly style?: Partial<CSSStyleDeclaration>;

  /** Lists of graph objects */
  readonly nodes: ReadonlyArray<Node>;
  readonly connections: ReadonlyArray<Connection>;
  readonly selectedNodeIds?: ReadonlySet<string>;
  readonly hideControls?: boolean;
  readonly fillHeight?: boolean;
  readonly toolbarItems?: m.Children;

  /** Called on every update cycle with the latest API object */
  readonly onReady?: (api: NodeGraphAPI) => void;

  /** Called when a new connection is made */
  readonly onConnect?: (connection: Connection) => void;

  /** Called when a connection is removed */
  readonly onDisconnect?: (index: number) => void;

  /** Called when a node is dragged and dropped */
  readonly onNodeMove?: (nodeId: string, x: number, y: number) => void;

  /** Called when one node is docked to another node */
  readonly onNodeDock?: (nodeId: string, targetId: string) => void;

  /** Called when the selection is changed entirely */
  readonly onSelect?: (nodeIds: string[]) => void;

  /** Called when a node is added to the current selection. */
  readonly onSelectionAdd?: (nodeId: string) => void;

  /** Called when a node is removed from the current selection. */
  readonly onSelectionRemove?: (nodeId: string) => void;

  /** Called when the selection is cleared entirely */
  readonly onSelectionClear?: () => void;

  onViewportMove?(viewport: NodeGraphViewport): void;
  onBlobMove?(blobId: string, x: number, y: number): void;

  // The initial viewport (pan/zoom) to apply when the graph is first rendered.
  // This is optional - if not provided, the graph starts with offset (0,0) and
  // zoom 1.0.
  readonly initialViewport?: NodeGraphViewport;
}
