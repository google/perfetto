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
