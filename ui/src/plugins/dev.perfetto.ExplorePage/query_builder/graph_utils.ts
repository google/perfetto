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

import {QueryNode} from '../query_node';

/**
 * Graph traversal utilities for the Explore Page query builder.
 * Consolidates graph traversal logic to eliminate code duplication.
 */

/**
 * Gets all nodes reachable from the given root nodes (both forward and backward).
 * Uses breadth-first traversal to avoid visiting the same node multiple times.
 *
 * @param rootNodes The starting nodes for traversal
 * @returns All reachable nodes (including root nodes)
 */
export function getAllNodes(rootNodes: QueryNode[]): QueryNode[] {
  const allNodes: QueryNode[] = [];
  const visited = new Set<string>();
  const queue = [...rootNodes];

  while (queue.length > 0) {
    const node = queue.shift()!;
    if (visited.has(node.nodeId)) {
      continue;
    }
    visited.add(node.nodeId);
    allNodes.push(node);

    // Traverse forward edges (next nodes)
    queue.push(...node.nextNodes);

    // Traverse backward edges (input nodes)
    if (node.primaryInput) {
      queue.push(node.primaryInput);
    }
    if (node.secondaryInputs) {
      for (const inputNode of node.secondaryInputs.connections.values()) {
        queue.push(inputNode);
      }
    }
  }

  return allNodes;
}

/**
 * Gets all nodes downstream from the given node (including the node itself).
 * Only traverses forward edges (nextNodes).
 *
 * @param node The starting node
 * @returns All downstream nodes (including the starting node)
 */
export function getAllDownstreamNodes(node: QueryNode): QueryNode[] {
  const downstreamNodes: QueryNode[] = [];
  const visited = new Set<string>();
  const queue: QueryNode[] = [node];

  while (queue.length > 0) {
    const current = queue.shift()!;
    if (visited.has(current.nodeId)) {
      continue;
    }
    visited.add(current.nodeId);
    downstreamNodes.push(current);

    // Only traverse forward edges
    queue.push(...current.nextNodes);
  }

  return downstreamNodes;
}

/**
 * Gets all nodes upstream from the given node (not including the node itself).
 * Only traverses backward edges (primaryInput and secondaryInputs).
 *
 * @param node The starting node
 * @returns All upstream nodes (excluding the starting node)
 */
export function getAllUpstreamNodes(node: QueryNode): QueryNode[] {
  const upstreamNodes: QueryNode[] = [];
  const visited = new Set<string>();
  const queue: QueryNode[] = [];

  // Add all input nodes to the queue
  if (node.primaryInput) {
    queue.push(node.primaryInput);
  }
  if (node.secondaryInputs) {
    for (const inputNode of node.secondaryInputs.connections.values()) {
      queue.push(inputNode);
    }
  }

  while (queue.length > 0) {
    const current = queue.shift()!;
    if (visited.has(current.nodeId)) {
      continue;
    }
    visited.add(current.nodeId);
    upstreamNodes.push(current);

    // Only traverse backward edges
    if (current.primaryInput) {
      queue.push(current.primaryInput);
    }
    if (current.secondaryInputs) {
      for (const inputNode of current.secondaryInputs.connections.values()) {
        queue.push(inputNode);
      }
    }
  }

  return upstreamNodes;
}

/**
 * Finds a node by its ID in the graph.
 *
 * @param nodeId The ID of the node to find
 * @param rootNodes The root nodes to start searching from
 * @returns The node if found, undefined otherwise
 */
export function findNodeById(
  nodeId: string,
  rootNodes: QueryNode[],
): QueryNode | undefined {
  const allNodes = getAllNodes(rootNodes);
  return allNodes.find((n) => n.nodeId === nodeId);
}

// ============================================================================
// Graph Manipulation Utilities
// ============================================================================

/**
 * Inserts a new node between a parent node and its children.
 * This is used when adding single-input operation nodes (like filters,
 * aggregations) that should be inserted into an existing pipeline.
 *
 * The operation:
 * 1. Disconnects parent from all its children
 * 2. Connects parent -> newNode
 * 3. Connects newNode -> each child
 *
 * **IMPORTANT**: This function mutates the parentNode's nextNodes array.
 *
 * @param parentNode The parent node (will be mutated)
 * @param newNode The node to insert
 * @param addConnection Function to add connections between nodes
 * @param removeConnection Function to remove connections between nodes
 */
export function insertNodeBetween(
  parentNode: QueryNode,
  newNode: QueryNode,
  addConnection: (from: QueryNode, to: QueryNode, portIndex?: number) => void,
  removeConnection: (from: QueryNode, to: QueryNode) => void,
): void {
  // Prevent self-referential insert
  if (parentNode === newNode) {
    throw new Error('Cannot insert a node between itself');
  }

  // Store the existing child nodes along with their connection info
  // We need to preserve the port index for secondary input connections
  const existingChildren: Array<{
    child: QueryNode;
    portIndex: number | undefined;
  }> = [];

  for (const child of parentNode.nextNodes) {
    if (child !== undefined) {
      // Check if parentNode is connected to child's secondary inputs
      // and find the port index if so
      let portIndex: number | undefined = undefined;
      if (child.secondaryInputs) {
        for (const [port, inputNode] of child.secondaryInputs.connections) {
          if (inputNode === parentNode) {
            portIndex = port;
            break;
          }
        }
      }
      existingChildren.push({child, portIndex});
    }
  }

  // Clear parent's next nodes (we'll reconnect through newNode)
  parentNode.nextNodes = [];

  // Connect: parent -> newNode
  addConnection(parentNode, newNode);

  // Connect: newNode -> each existing child, preserving port indices
  for (const {child, portIndex} of existingChildren) {
    // Remove old connection from parent to child (if it still exists)
    removeConnection(parentNode, child);
    // Add connection from newNode to child, preserving the port index
    addConnection(newNode, child, portIndex);
  }
}

/**
 * Reconnects parent nodes to child nodes, bypassing a node being deleted.
 * Used when removing a node from the graph to maintain connectivity.
 *
 * IMPORTANT: This function preserves port indices from the deleted node to its
 * children. If the deleted node was connected to a child's secondary input,
 * the parent will also be connected to that child's secondary input.
 *
 * If either parentNodes or childConnectionInfo is empty, this function becomes
 * a no-op (no connections are created). This is expected behavior when deleting
 * terminal nodes (no parents or no children).
 *
 * @param parentNodes The parent nodes to reconnect (empty array is valid)
 * @param childConnectionInfo Array of children with their port index information
 * @param addConnection Function to add connections between nodes
 */
export function reconnectParentsToChildren(
  parentNodes: QueryNode[],
  childConnectionInfo: Array<{child: QueryNode; portIndex: number | undefined}>,
  addConnection: (from: QueryNode, to: QueryNode, portIndex?: number) => void,
): void {
  for (const parent of parentNodes) {
    for (const {child, portIndex} of childConnectionInfo) {
      addConnection(parent, child, portIndex);
    }
  }
}

// ============================================================================
// Node Navigation Utilities
// ============================================================================

/**
 * Gets the input node at a specific port index.
 * Only applicable for nodes with secondary inputs (multi-source nodes).
 *
 * @param node The node to get input from
 * @param portIndex The port index
 * @returns The input node at that port, or undefined if not found
 */
export function getInputNodeAtPort(
  node: QueryNode,
  portIndex: number,
): QueryNode | undefined {
  if ('secondaryInputs' in node && node.secondaryInputs) {
    return node.secondaryInputs.connections.get(portIndex);
  }
  return undefined;
}

/**
 * Gets all parent nodes (both primary and secondary inputs).
 * This is useful for finding all nodes that feed into a given node.
 *
 * @param node The node to get parents for
 * @returns Array of all parent nodes
 */
export function getAllInputNodes(node: QueryNode): QueryNode[] {
  const inputs: QueryNode[] = [];

  if ('primaryInput' in node && node.primaryInput) {
    inputs.push(node.primaryInput);
  }

  if ('secondaryInputs' in node && node.secondaryInputs) {
    for (const inputNode of node.secondaryInputs.connections.values()) {
      inputs.push(inputNode);
    }
  }

  return inputs;
}
