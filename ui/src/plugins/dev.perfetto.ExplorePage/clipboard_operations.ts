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

import {QueryNode} from './query_node';
import {getAllNodes, addConnection} from './query_builder/graph_utils';

// Clipboard entry stores a cloned node with its relative position for paste.
export interface ClipboardEntry {
  node: QueryNode;
  relativeX: number; // Position relative to the first node (only used if not docked)
  relativeY: number;
  isDocked: boolean; // True if node was docked (no explicit layout position)
}

// Clipboard connection stores connections between clipboard nodes (by index).
export interface ClipboardConnection {
  fromIndex: number;
  toIndex: number;
  portIndex?: number;
}

export interface ClipboardResult {
  clipboardNodes: ClipboardEntry[];
  clipboardConnections: ClipboardConnection[];
}

// The subset of state needed for copy operations.
interface CopyableState {
  readonly rootNodes: QueryNode[];
  readonly selectedNodes: ReadonlySet<string>;
  readonly nodeLayouts: Map<string, {x: number; y: number}>;
}

// The subset of state needed for paste operations.
interface PastableState {
  readonly rootNodes: QueryNode[];
  readonly nodeLayouts: Map<string, {x: number; y: number}>;
  readonly clipboardNodes?: ClipboardEntry[];
  readonly clipboardConnections?: ClipboardConnection[];
}

// Copies the currently selected nodes and their internal connections to a
// clipboard result. Returns undefined if no nodes are selected.
export function copySelectedNodes(
  state: CopyableState,
): ClipboardResult | undefined {
  const selectedNodeIds = state.selectedNodes;

  if (selectedNodeIds.size === 0) {
    return undefined;
  }

  const allNodes = getAllNodes(state.rootNodes);
  const selectedNodes = allNodes.filter((n) => selectedNodeIds.has(n.nodeId));

  if (selectedNodes.length === 0) {
    return undefined;
  }

  // Get positions for relative layout calculation
  const positions = selectedNodes.map((node) => {
    const layout = state.nodeLayouts.get(node.nodeId);
    return {
      node,
      x: layout?.x ?? 0,
      y: layout?.y ?? 0,
    };
  });

  // Find the top-left corner as reference point
  const minX = Math.min(...positions.map((p) => p.x));
  const minY = Math.min(...positions.map((p) => p.y));

  // Create clipboard entries with cloned nodes and relative positions
  // Track whether each node is docked (no explicit layout) or undocked
  const nodeIdToIndex = new Map<string, number>();
  const clipboardNodes: ClipboardEntry[] = positions.map((p, index) => {
    nodeIdToIndex.set(p.node.nodeId, index);
    const hasLayout = state.nodeLayouts.has(p.node.nodeId);
    return {
      node: p.node.clone(),
      relativeX: p.x - minX,
      relativeY: p.y - minY,
      isDocked: !hasLayout,
    };
  });

  // Capture connections between selected nodes
  const clipboardConnections: ClipboardConnection[] = [];
  for (const node of selectedNodes) {
    const toIndex = nodeIdToIndex.get(node.nodeId);
    if (toIndex === undefined) continue;

    // Check primaryInput
    if (node.primaryInput && selectedNodeIds.has(node.primaryInput.nodeId)) {
      const fromIndex = nodeIdToIndex.get(node.primaryInput.nodeId);
      if (fromIndex !== undefined) {
        clipboardConnections.push({fromIndex, toIndex});
      }
    }

    // Check secondaryInputs
    if (node.secondaryInputs) {
      for (const [portIndex, inputNode] of node.secondaryInputs.connections) {
        if (selectedNodeIds.has(inputNode.nodeId)) {
          const fromIndex = nodeIdToIndex.get(inputNode.nodeId);
          if (fromIndex !== undefined) {
            clipboardConnections.push({fromIndex, toIndex, portIndex});
          }
        }
      }
    }
  }

  return {clipboardNodes, clipboardConnections};
}

// Pastes clipboard nodes into the state. Returns the updated state fields
// with new nodes added, or undefined if clipboard is empty.
export function pasteClipboardNodes(state: PastableState):
  | {
      rootNodes: QueryNode[];
      selectedNodes: Set<string>;
      nodeLayouts: Map<string, {x: number; y: number}>;
    }
  | undefined {
  if (state.clipboardNodes === undefined || state.clipboardNodes.length === 0) {
    return undefined;
  }

  // Clone nodes again for this paste operation (allows multiple pastes)
  const newNodes = state.clipboardNodes.map((entry) => entry.node.clone());

  // Calculate paste offset (place slightly offset from original)
  const pasteOffsetX = 50;
  const pasteOffsetY = 50;

  // Update layouts for new nodes - only add layouts for undocked nodes
  // Docked nodes will remain docked (attached to their parent)
  const updatedLayouts = new Map(state.nodeLayouts);
  state.clipboardNodes.forEach((entry, index) => {
    if (!entry.isDocked) {
      updatedLayouts.set(newNodes[index].nodeId, {
        x: entry.relativeX + pasteOffsetX,
        y: entry.relativeY + pasteOffsetY,
      });
    }
  });

  // Restore connections between pasted nodes
  if (state.clipboardConnections) {
    for (const conn of state.clipboardConnections) {
      const fromNode = newNodes[conn.fromIndex] as QueryNode | undefined;
      const toNode = newNodes[conn.toIndex] as QueryNode | undefined;
      if (fromNode !== undefined && toNode !== undefined) {
        addConnection(fromNode, toNode, conn.portIndex);
      }
    }
  }

  return {
    rootNodes: [...state.rootNodes, ...newNodes],
    selectedNodes: new Set(newNodes.map((n) => n.nodeId)),
    nodeLayouts: updatedLayouts,
  };
}
