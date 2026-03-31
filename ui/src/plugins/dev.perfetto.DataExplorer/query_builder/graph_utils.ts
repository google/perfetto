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

import {
  QueryNode,
  NodeType,
  SecondaryInputSpec,
  singleNodeOperation,
} from '../query_node';
import {GroupNode, ExternalGroupConnection} from './nodes/group_node';
import {Result, errResult, okResult} from '../../../base/result';

/**
 * Graph traversal and connection utilities for the Data Explorer query builder.
 * Consolidates graph traversal logic to eliminate code duplication.
 */

/**
 * Gets all nodes reachable from the given root nodes (both forward and backward).
 * Uses breadth-first traversal to avoid visiting the same node multiple times.
 *
 * By default, when a GroupNode is encountered, its inner nodes are also
 * included in the result (they are not in rootNodes but are owned by the
 * group). Pass `traverseGroups: false` to skip inner nodes — useful for
 * rendering where only outer-graph nodes should be visible.
 *
 * @param rootNodes The starting nodes for traversal
 * @returns All reachable nodes (including root nodes)
 */
export function getAllNodes(
  rootNodes: QueryNode[],
  opts?: {traverseGroups?: boolean},
): QueryNode[] {
  const traverseGroups = opts?.traverseGroups ?? true;
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

    // Traverse into group inner nodes so they are discoverable even
    // though they are not in rootNodes.
    if (traverseGroups && node instanceof GroupNode) {
      queue.push(...node.innerNodes);
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
  // Note: A parent can be connected to a child on multiple ports (e.g., Union node)
  const existingChildren = captureAllChildConnections(parentNode);

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

/**
 * Captures all connections from a parent to its children, including multiple
 * connections to the same child on different ports.
 */
export function captureAllChildConnections(
  parentNode: QueryNode,
): Array<{child: QueryNode; portIndex: number | undefined}> {
  const connections: Array<{child: QueryNode; portIndex: number | undefined}> =
    [];

  for (const child of parentNode.nextNodes) {
    // Check primary input connection
    if (child.primaryInput === parentNode) {
      connections.push({child, portIndex: undefined});
    }

    // Check all secondary input connections
    if (child.secondaryInputs) {
      for (const [port, inputNode] of child.secondaryInputs.connections) {
        if (inputNode === parentNode) {
          connections.push({child, portIndex: port});
        }
      }
    }
  }

  return connections;
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

// ============================================================================
// Docking/Undocking Utilities
// ============================================================================

/**
 * Returns true if a node is undocked (has an explicit layout position).
 * Undocked nodes are rendered as separate graph nodes rather than inline
 * with their parent's chain.
 */
export function isNodeUndocked(
  node: QueryNode,
  nodeLayouts: ReadonlyMap<string, {x: number; y: number}>,
): boolean {
  return nodeLayouts.has(node.nodeId);
}

/**
 * Finds children of a node that are currently docked (rendered inline with parent).
 *
 * A child is considered docked if:
 * 1. It's a single-node operation (modification node like filter, sort, etc.)
 * 2. Its primaryInput is the parent node
 * 3. It doesn't have a layout position (no entry in nodeLayouts)
 *
 * @param parentNode The parent node to check
 * @param nodeLayouts Current layout positions
 * @returns Array of children that are currently docked to the parent
 */
export function findDockedChildren(
  parentNode: QueryNode,
  nodeLayouts: ReadonlyMap<string, {x: number; y: number}>,
): QueryNode[] {
  return parentNode.nextNodes.filter(
    (child) =>
      singleNodeOperation(child.type) &&
      'primaryInput' in child &&
      child.primaryInput === parentNode &&
      !nodeLayouts.has(child.nodeId),
  );
}

/**
 * Gets the effective layout position for a node by walking up the chain.
 *
 * If the node has its own layout, returns that. Otherwise, recursively
 * walks up through primaryInput to find the first ancestor with a layout.
 * This is useful for docked nodes that don't have their own layout position.
 *
 * @param node The node to get the effective layout for
 * @param nodeLayouts Current layout positions
 * @returns The effective layout position, or undefined if no ancestor has a layout
 */
export function getEffectiveLayout(
  node: QueryNode,
  nodeLayouts: ReadonlyMap<string, {x: number; y: number}>,
): {x: number; y: number} | undefined {
  // If this node has a layout, return it
  const directLayout = nodeLayouts.get(node.nodeId);
  if (directLayout !== undefined) {
    return directLayout;
  }

  // Otherwise, walk up the chain via primaryInput
  if ('primaryInput' in node && node.primaryInput !== undefined) {
    return getEffectiveLayout(node.primaryInput, nodeLayouts);
  }

  return undefined;
}

// Layout offset constants for undocking
const UNDOCK_X_OFFSET = 250;
const UNDOCK_STAGGER = 30;

/**
 * Calculates layout positions for undocking children from a parent.
 * Positions are staggered diagonally from the parent's position.
 *
 * @param children The children to calculate positions for
 * @param parentLayout The parent's layout position
 * @param parentLayout.x The parent's x coordinate
 * @param parentLayout.y The parent's y coordinate
 * @returns Map of child nodeId to new layout position
 */
export function calculateUndockLayouts(
  children: QueryNode[],
  parentLayout: {x: number; y: number},
): Map<string, {x: number; y: number}> {
  const layouts = new Map<string, {x: number; y: number}>();

  for (let i = 0; i < children.length; i++) {
    layouts.set(children[i].nodeId, {
      x: parentLayout.x + UNDOCK_X_OFFSET + i * UNDOCK_STAGGER,
      y: parentLayout.y + i * UNDOCK_STAGGER,
    });
  }

  return layouts;
}

/**
 * Computes updated layout positions that undock the given children from a
 * parent node.  Merges the new positions into `existingLayouts` and returns
 * the updated map.
 *
 * This is the shared logic used whenever we need to convert docked children
 * into undocked ones (e.g. when adding a new sibling node or a multi-source
 * node).
 *
 * @param parentNode      The parent whose children are being undocked.
 * @param childrenToUndock Nodes that need explicit layout positions.
 * @param existingLayouts  Current layout map (will be shallow-copied).
 * @returns A new Map with the undock positions merged in, or the original map
 *          if no positions could be computed (parent has no effective layout).
 */
export function applyUndockLayouts(
  parentNode: QueryNode,
  childrenToUndock: QueryNode[],
  existingLayouts: ReadonlyMap<string, {x: number; y: number}>,
): Map<string, {x: number; y: number}> {
  const updatedLayouts = new Map(existingLayouts);
  if (childrenToUndock.length === 0) {
    return updatedLayouts;
  }

  const effectiveLayout = getEffectiveLayout(parentNode, existingLayouts);
  if (effectiveLayout !== undefined) {
    const undockLayouts = calculateUndockLayouts(
      childrenToUndock,
      effectiveLayout,
    );
    for (const [nodeId, layout] of undockLayouts) {
      updatedLayouts.set(nodeId, layout);
    }
  }

  return updatedLayouts;
}

// ============================================================================
// Graph Connection Operations
// ============================================================================
// These functions encapsulate the bidirectional relationship management
// between nodes, ensuring consistency when adding/removing connections.

/**
 * Notifies all downstream nodes that their inputs have changed.
 */
export function notifyNextNodes(node: QueryNode): void {
  for (const nextNode of node.nextNodes) {
    nextNode.onPrevNodesUpdated?.();
  }
}

/**
 * Helper: Get secondary input at specific port
 */
export function getSecondaryInput(
  node: QueryNode,
  portIndex: number,
): QueryNode | undefined {
  return node.secondaryInputs?.connections.get(portIndex);
}

/**
 * Helper: Set secondary input at specific port
 */
export function setSecondaryInput(
  node: QueryNode,
  portIndex: number,
  inputNode: QueryNode,
): void {
  if (!node.secondaryInputs) {
    throw new Error('Node does not support secondary inputs');
  }
  node.secondaryInputs.connections.set(portIndex, inputNode);
}

/**
 * Helper: Remove secondary input at specific port
 */
export function removeSecondaryInput(node: QueryNode, portIndex: number): void {
  if (!node.secondaryInputs) return;
  node.secondaryInputs.connections.delete(portIndex);
}

/**
 * Validates that secondary inputs meet cardinality requirements.
 * Returns an error message if validation fails, undefined if valid.
 */
export function validateSecondaryInputs(node: QueryNode): string | undefined {
  if (!node.secondaryInputs) {
    return undefined;
  }

  const {connections, min, max}: SecondaryInputSpec = node.secondaryInputs;
  const count = connections.size;

  if (count < min) {
    return `Requires at least ${min} input${min === 1 ? '' : 's'}, but only ${count} connected`;
  }

  if (max !== 'unbounded' && count > max) {
    return `Allows at most ${max} input${max === 1 ? '' : 's'}, but ${count} connected`;
  }

  return undefined;
}

/**
 * Returns true if adding a connection from `fromNode` to `toNode` would
 * create a cycle in the graph. A cycle exists if `fromNode` is reachable
 * from `toNode` via forward edges (i.e., `fromNode` is already downstream
 * of `toNode`), or if `fromNode === toNode` (self-loop).
 */
export function wouldCreateCycle(
  fromNode: QueryNode,
  toNode: QueryNode,
): boolean {
  if (fromNode === toNode) return true;
  const downstream = getAllDownstreamNodes(toNode);
  return downstream.includes(fromNode);
}

/**
 * Adds a connection from one node to another, updating both forward and
 * backward links. For multi-source nodes, adds to the specified port index.
 */
export function addConnection(
  fromNode: QueryNode,
  toNode: QueryNode,
  portIndex?: number,
): void {
  // Update forward link (fromNode -> toNode)
  if (!fromNode.nextNodes.includes(toNode)) {
    fromNode.nextNodes.push(toNode);
  }

  // Determine connection type based on node characteristics
  if (singleNodeOperation(toNode.type)) {
    // Single-input operation node (Filter, Sort, etc.)
    // If portIndex is specified, connect to secondary input
    if (portIndex !== undefined) {
      if (!toNode.secondaryInputs) {
        throw new Error(
          `Node ${toNode.nodeId} does not support secondary inputs`,
        );
      }
      setSecondaryInput(toNode, portIndex, fromNode);
    } else {
      // Otherwise connect to primary input (default from above)
      toNode.primaryInput = fromNode;
    }
    toNode.onPrevNodesUpdated?.();
  } else if (toNode.secondaryInputs) {
    // Multi-source node (Union, Join, IntervalIntersect)
    if (portIndex !== undefined) {
      // Set at specific port
      setSecondaryInput(toNode, portIndex, fromNode);
    } else {
      // Find first available port
      let nextPort = 0;
      while (toNode.secondaryInputs.connections.has(nextPort)) {
        nextPort++;
      }
      setSecondaryInput(toNode, nextPort, fromNode);
    }
    toNode.onPrevNodesUpdated?.();
  }
}

/**
 * Removes a connection from one node to another, cleaning up both forward
 * and backward links.
 */
export function removeConnection(
  fromNode: QueryNode,
  toNode: QueryNode,
  specificPort?: number,
): void {
  // Check if it's in primary input
  if (toNode.primaryInput === fromNode) {
    toNode.primaryInput = undefined;
    toNode.onPrevNodesUpdated?.();
  }

  // Check if it's in secondary inputs
  if (toNode.secondaryInputs) {
    if (specificPort !== undefined) {
      // Remove specific port connection
      const inputNode = toNode.secondaryInputs.connections.get(specificPort);
      if (inputNode === fromNode) {
        removeSecondaryInput(toNode, specificPort);
        toNode.onPrevNodesUpdated?.();
      }
    } else {
      // No specific port - remove ALL connections from fromNode to toNode
      const portsToRemove: number[] = [];
      for (const [portIndex, inputNode] of toNode.secondaryInputs.connections) {
        if (inputNode === fromNode) {
          portsToRemove.push(portIndex);
        }
      }
      for (const port of portsToRemove) {
        removeSecondaryInput(toNode, port);
      }
      if (portsToRemove.length > 0) {
        toNode.onPrevNodesUpdated?.();
      }
    }
  }

  // Only remove from nextNodes if no connections remain from fromNode to toNode
  const stillConnected =
    toNode.primaryInput === fromNode ||
    (toNode.secondaryInputs &&
      Array.from(toNode.secondaryInputs.connections.values()).includes(
        fromNode,
      ));

  if (!stillConnected) {
    const nextIndex = fromNode.nextNodes.indexOf(toNode);
    if (nextIndex !== -1) {
      fromNode.nextNodes.splice(nextIndex, 1);
    }
  }
}

// ============================================================================
// Group Creation
// ============================================================================

/**
 * Attempts to create a GroupNode from a set of selected node IDs.
 *
 * Validation rules:
 *  - At least 2 nodes must be selected.
 *  - Exactly one "end node" must exist: an inner node whose nextNodes are all
 *    outside the selection (i.e. the group has a single output).
 *
 * On success the function:
 *  1. Creates a GroupNode that proxies SQL generation to the end node.
 *  2. Rewires external source nodes to point to the GroupNode instead of
 *     the inner targets (so inner nodes disappear from the outer graph).
 *  3. Rewires outer nodes (previously connected after the end node) to use
 *     the GroupNode as their input.
 *
 * The inner nodes retain their original primaryInput / secondaryInputs
 * references so that SQL generation continues to work transitively.
 *
 * @returns The new GroupNode on success, or an error result on failure.
 */
export function createGroupFromSelection(
  selectedNodeIds: ReadonlySet<string>,
  allNodes: readonly QueryNode[],
): Result<GroupNode> {
  const innerNodes = allNodes.filter((n) => selectedNodeIds.has(n.nodeId));

  if (innerNodes.length < 2) {
    return errResult('Select at least 2 nodes to create a group.');
  }

  // Reject selections that include existing group nodes — nesting is not
  // supported because the inner rewiring and SQL proxy assumptions break down.
  if (innerNodes.some((n) => n.type === NodeType.kGroup)) {
    return errResult(
      'Cannot create group: selection contains an existing group. Ungroup it first.',
    );
  }

  const innerSet = new Set(innerNodes.map((n) => n.nodeId));

  // The end node is the only inner node whose children are all outside the group.
  const endNodes = innerNodes.filter(
    (n) => !n.nextNodes.some((next) => innerSet.has(next.nodeId)),
  );

  if (endNodes.length === 0) {
    return errResult(
      'Cannot create group: the selected nodes form a cycle with no output.',
    );
  }
  if (endNodes.length > 1) {
    return errResult(
      `Cannot create group: found ${endNodes.length} output nodes. ` +
        'The group must have exactly one output node.',
    );
  }

  const endNode = endNodes[0];

  // Collect all external connections (sources outside the group → inner nodes).
  const externalConnections: ExternalGroupConnection[] = [];
  let portCounter = 0;

  for (const inner of innerNodes) {
    if (
      inner.primaryInput !== undefined &&
      !innerSet.has(inner.primaryInput.nodeId)
    ) {
      externalConnections.push({
        sourceNode: inner.primaryInput,
        innerTargetNode: inner,
        innerTargetPort: undefined,
        groupPort: portCounter++,
      });
    }
    if (inner.secondaryInputs !== undefined) {
      for (const [port, source] of inner.secondaryInputs.connections) {
        if (source === undefined) continue;
        if (!innerSet.has(source.nodeId)) {
          externalConnections.push({
            sourceNode: source,
            innerTargetNode: inner,
            innerTargetPort: port,
            groupPort: portCounter++,
          });
        }
      }
    }
  }

  // Outer nodes: endNode's children that are outside the group.
  const outerNodes = endNode.nextNodes.filter((n) => !innerSet.has(n.nodeId));

  // Create the GroupNode (no mutations yet).
  const groupNode = new GroupNode(
    {name: 'Group'},
    {},
    innerNodes,
    endNode,
    externalConnections,
  );
  groupNode.nextNodes = [...outerNodes];

  return okResult(groupNode);
}

/**
 * Applies the graph rewiring needed after creating a GroupNode.
 * This mutates the existing nodes (sourceNode.nextNodes, outerNode.primaryInput,
 * etc.) so it should only be called inside a state update callback where the
 * mutations are expected.
 */
export function applyGroupRewiring(groupNode: GroupNode): void {
  const endNode = groupNode.endNode;
  if (endNode === undefined) return;

  const innerSet = new Set(groupNode.innerNodes.map((n) => n.nodeId));

  // Rewire: each external source now points to GroupNode instead of inner targets.
  // Use filter+push instead of per-connection splice to avoid index-shift bugs
  // when a single source feeds multiple inner nodes.
  const sourcesToRewire = new Set(
    groupNode.externalConnections.map((c) => c.sourceNode),
  );
  const innerTargets = new Set(
    groupNode.externalConnections.map((c) => c.innerTargetNode),
  );
  for (const source of sourcesToRewire) {
    source.nextNodes = source.nextNodes.filter((n) => !innerTargets.has(n));
    if (!source.nextNodes.includes(groupNode)) {
      source.nextNodes.push(groupNode);
    }
  }

  // Rewire: outer nodes now reference GroupNode as their input instead of endNode.
  for (const outerNode of groupNode.nextNodes) {
    if (outerNode.primaryInput === endNode) {
      outerNode.primaryInput = groupNode;
    }
    if (outerNode.secondaryInputs !== undefined) {
      for (const [port, src] of outerNode.secondaryInputs.connections) {
        if (src === endNode) {
          outerNode.secondaryInputs.connections.set(port, groupNode);
        }
      }
    }
  }

  // endNode no longer has direct outer connections; GroupNode owns them.
  // Mutate in-place so existing references to the array stay consistent.
  for (let i = endNode.nextNodes.length - 1; i >= 0; i--) {
    if (!innerSet.has(endNode.nextNodes[i].nodeId)) {
      endNode.nextNodes.splice(i, 1);
    }
  }
}

/**
 * Dissolves a GroupNode, restoring the inner nodes to the outer graph.
 * This is the inverse of createGroupFromSelection + applyGroupRewiring.
 *
 * Rewiring performed:
 *  1. External sources that point to the GroupNode are redirected back to
 *     the original inner target nodes.
 *  2. Outer nodes (GroupNode.nextNodes) that reference the GroupNode as
 *     their input are redirected to the end node.
 *  3. The end node's nextNodes are restored to include the outer nodes.
 */
export function ungroupNode(groupNode: GroupNode): void {
  const endNode = groupNode.endNode;

  // 1. Rewire external sources: point back to inner targets instead of group.
  // Collect per-source: remove group, add back inner targets.
  const sourcesToRestore = new Map<QueryNode, QueryNode[]>();
  for (const conn of groupNode.externalConnections) {
    const targets = sourcesToRestore.get(conn.sourceNode) ?? [];
    targets.push(conn.innerTargetNode);
    sourcesToRestore.set(conn.sourceNode, targets);
  }
  for (const [source, innerTargets] of sourcesToRestore) {
    source.nextNodes = source.nextNodes.filter((n) => n !== groupNode);
    for (const target of innerTargets) {
      if (!source.nextNodes.includes(target)) {
        source.nextNodes.push(target);
      }
    }
  }

  // 2. Rewire outer nodes: replace GroupNode input with endNode.
  if (endNode !== undefined) {
    for (const outerNode of groupNode.nextNodes) {
      if (outerNode.primaryInput === groupNode) {
        outerNode.primaryInput = endNode;
      }
      if (outerNode.secondaryInputs !== undefined) {
        for (const [port, src] of outerNode.secondaryInputs.connections) {
          if (src === groupNode) {
            outerNode.secondaryInputs.connections.set(port, endNode);
          }
        }
      }
    }

    // 3. Restore endNode's outer connections.
    for (const outerNode of groupNode.nextNodes) {
      if (!endNode.nextNodes.includes(outerNode)) {
        endNode.nextNodes.push(outerNode);
      }
    }
  }
}
