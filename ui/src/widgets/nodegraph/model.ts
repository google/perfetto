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
import {Point2D} from '../../base/geom';

export interface NodeGraphConnection {
  readonly fromPort: string;
  readonly toPort: string;
}

export type Direction = 'north' | 'south' | 'east' | 'west';

export interface NodeGraphPort {
  // Unique identifier for this port.
  readonly id: string;

  // Label shown next to the port dot. Only displayed for east/west ports.
  readonly label?: m.Children;

  // Which edge of the node this port appears on.
  readonly direction: Direction;

  // Items shown in a context menu when the port is right-clicked or tapped.
  readonly contextMenuItems?: m.Children;
}

// A node that is docked (stacked) below another node. Identical to
// NodeGraphNode but without a position, since its parent determines placement.
export type NodeGraphDockedNode = Omit<NodeGraphNode, 'pos'>;

export interface NodeGraphNode {
  // Unique identifier for this node.
  readonly id: string;

  // Position in canvas coordinates (independent of the current pan/zoom).
  readonly pos: Point2D;

  // Hue (0–360) used to tint the header and accent bar. The rendered color
  // adapts to the current light/dark theme.
  readonly hue: number;

  // Renders a colored strip on the left edge of the card. Mutually exclusive
  // with headerBar — they don't look good together.
  readonly accentBar?: boolean;

  // Renders a title bar at the top of the card with an optional icon. Mutually
  // exclusive with accentBar — they don't look good together.
  readonly headerBar?: {
    readonly title: m.Children;
    readonly icon?: string;
  };

  // Input ports shown on the node (used to receive connections).
  readonly inputs?: ReadonlyArray<NodeGraphPort>;

  // Output ports shown on the node (used to originate connections).
  readonly outputs?: ReadonlyArray<NodeGraphPort>;

  // Arbitrary content rendered in the card body.
  readonly content?: m.Children;

  // Next node stacked below this one. When set, the two nodes are rendered as
  // a single visual unit and move together.
  readonly next?: NodeGraphDockedNode;

  // When true, this node can be dragged and docked below a node that has
  // canDockBottom set.
  readonly canDockTop?: boolean;

  // When true, another node with canDockTop can be docked below this one.
  readonly canDockBottom?: boolean;

  // Items shown in a context menu on the node (e.g. via a triple-dot button).
  readonly contextMenuItems?: m.Children;

  // Extra CSS class(es) applied to the outermost node element.
  readonly className?: string;
}

export interface NodeGraphAPI {
  // Adjusts pan and zoom so all nodes fit within the visible viewport.
  autofit(): void;

  // Pans the canvas by (dx, dy) in viewport pixels. Positive values pan right
  // and down.
  pan(dx: number, dy: number): void;

  // Zooms by adding deltaZoom to the current zoom level (e.g. +0.1 zooms in).
  // centerX/centerY are the fixed point in viewport coordinates; defaults to
  // the canvas center.
  zoom(deltaZoom: number, centerX?: number, centerY?: number): void;

  // Resets zoom to 1.0, keeping the current viewport center fixed.
  resetZoom(): void;

  // Finds a canvas position for a new node that doesn't overlap any existing
  // node. Starts from the viewport center and spirals outward. The node
  // description is used for context but the position is estimated since the
  // node hasn't been rendered yet.
  findPlacementForNode(node: Omit<NodeGraphNode, 'pos'>): Point2D;
}

export interface NodeGraphAttrs {
  readonly className?: string;
  readonly style?: Partial<CSSStyleDeclaration>;

  // The nodes and connections to render.
  readonly nodes: ReadonlyArray<NodeGraphNode>;
  readonly connections: ReadonlyArray<NodeGraphConnection>;

  // IDs of currently selected nodes. The graph highlights these but does not
  // own the selection state — the parent is responsible for updating it.
  readonly selectedNodeIds?: ReadonlySet<string>;

  // Hide the built-in zoom/fit toolbar.
  readonly hideControls?: boolean;

  // Make the graph fill the height of its container.
  readonly fillHeight?: boolean;

  // Extra items rendered in the toolbar alongside the built-in controls.
  readonly toolbarItems?: m.Children;

  // Called once after the component mounts, with an API object for
  // programmatic control (pan, zoom, placement, etc.).
  readonly onReady?: (api: NodeGraphAPI) => void;

  // Called when the user draws a wire between two ports.
  readonly onConnect?: (connection: NodeGraphConnection) => void;

  // Called when the user removes a connection. The index refers to the
  // position of the connection in the `connections` array.
  readonly onDisconnect?: (index: number) => void;

  // Called when the user drops a node at a new position. The position is
  // already snapped to the grid and adjusted to avoid overlaps.
  readonly onNodeMove?: (nodeId: string, pos: Point2D) => void;

  // Called when the user docks one node below another.
  readonly onNodeDock?: (nodeId: string, targetId: string) => void;

  // Called when the user triggers deletion of nodes (e.g. toolbar delete
  // button). Receives the IDs of all nodes to remove.
  readonly onNodeRemove?: (nodeIds: string[]) => void;

  // Called when the selection is replaced entirely (e.g. click or box-select).
  readonly onSelect?: (nodeIds: string[]) => void;

  // Called when a single node is added to the existing selection (Ctrl+click).
  readonly onSelectionAdd?: (nodeId: string) => void;

  // Called when a single node is removed from the selection (Ctrl+click on a
  // selected node).
  readonly onSelectionRemove?: (nodeId: string) => void;

  // Called when the selection is cleared (e.g. clicking the empty canvas).
  readonly onSelectionClear?: () => void;

  // Called whenever the viewport changes due to pan or zoom.
  readonly onViewportMove?: (viewport: NodeGraphViewport) => void;

  // Initial pan/zoom applied when the component first mounts. Defaults to
  // offset (0, 0) and zoom 1.0.
  readonly initialViewport?: NodeGraphViewport;
}
