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

import {Trace} from '../../public/trace';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {CleanupManager} from './query_builder/cleanup_manager';
import type {NodeActionHandlers} from './node_actions';
import {createDeferredNodeActions} from './node_actions';
import {QueryNode, QueryNodeState, singleNodeOperation} from './query_node';
import {nodeRegistry, type PreCreateState} from './query_builder/node_registry';
import {
  getAllNodes,
  insertNodeBetween,
  getInputNodeAtPort,
  getAllInputNodes,
  findDockedChildren,
  calculateUndockLayouts,
  getEffectiveLayout,
  addConnection,
  removeConnection,
  notifyNextNodes,
  captureAllChildConnections,
} from './query_builder/graph_utils';
import {ExplorePageState} from './explore_page';

// Dependencies needed by node CRUD operations.
export interface NodeCrudDeps {
  readonly trace: Trace;
  readonly sqlModules: SqlModules;
  readonly onStateUpdate: (
    update:
      | ExplorePageState
      | ((currentState: ExplorePageState) => ExplorePageState),
  ) => void;
  readonly cleanupManager?: CleanupManager;
  readonly initializedNodes: Set<string>;
  readonly nodeActionHandlers: NodeActionHandlers;
}

// Gets the primary input parent of a node.
// Returns undefined for source nodes and multi-source nodes.
function getPrimaryParent(node: QueryNode): QueryNode | undefined {
  if ('primaryInput' in node) {
    return node.primaryInput;
  }
  return undefined;
}

// Disconnects a node from all its parents and children.
function disconnectNodeFromGraph(node: QueryNode): void {
  const allParents = getAllInputNodes(node);
  for (const parent of allParents) {
    removeConnection(parent, node);
  }
  const children = [...node.nextNodes];
  for (const child of children) {
    removeConnection(node, child);
  }
}

// Cleans up all existing nodes (drops materialized tables) and clears
// the initialized nodes set. Used when replacing the entire graph state.
export async function cleanupExistingNodes(
  cleanupManager: CleanupManager | undefined,
  initializedNodes: Set<string>,
  rootNodes: QueryNode[],
): Promise<void> {
  if (cleanupManager !== undefined) {
    const allNodes = getAllNodes(rootNodes);
    await cleanupManager.cleanupNodes(allNodes);
  }
  initializedNodes.clear();
}

export async function addOperationNode(
  deps: NodeCrudDeps,
  state: ExplorePageState,
  parentNode: QueryNode,
  derivedNodeId: string,
): Promise<QueryNode | undefined> {
  const descriptor = nodeRegistry.get(derivedNodeId);
  if (descriptor) {
    let initialState: PreCreateState | PreCreateState[] | null = {};
    if (descriptor.preCreate) {
      initialState = await descriptor.preCreate({
        sqlModules: deps.sqlModules,
      });
    }

    if (initialState === null) {
      return;
    }

    // For operation nodes, we only support single node creation
    // (multi-select only makes sense for source nodes)
    if (Array.isArray(initialState)) {
      console.warn(
        'Operation nodes do not support multi-node creation from preCreate',
      );
      return;
    }

    // Use a wrapper object to hold the node reference (allows mutation without 'let')
    const nodeRef: {current?: QueryNode} = {};

    const nodeState: QueryNodeState = {
      ...(initialState as Partial<QueryNodeState>),
      sqlModules: deps.sqlModules,
      trace: deps.trace,
      // Provide actions for nodes that need to interact with the graph
      // We use a deferred pattern because the node doesn't exist yet
      actions: createDeferredNodeActions(nodeRef, deps.nodeActionHandlers),
    };

    const newNode = descriptor.factory(nodeState, {
      allNodes: state.rootNodes,
    });

    // Set the reference so the callback can use it
    nodeRef.current = newNode;

    // Mark this node as initialized
    deps.initializedNodes.add(newNode.nodeId);

    if (singleNodeOperation(newNode.type)) {
      // For single-input operations: insert between the target and its children
      insertNodeBetween(parentNode, newNode, addConnection, removeConnection);

      deps.onStateUpdate((currentState) => ({
        ...currentState,
        selectedNodes: new Set([newNode.nodeId]),
      }));
    } else {
      // For multi-source nodes: just connect and add to root nodes
      // Don't insert in-between - the node combines multiple sources

      // Undock docked children before adding (docking requires exactly one child)
      const dockedChildren = findDockedChildren(parentNode, state.nodeLayouts);

      addConnection(parentNode, newNode);

      deps.onStateUpdate((currentState) => {
        const updatedLayouts = new Map(currentState.nodeLayouts);

        // Undock existing docked children by giving them layouts.
        // Use getEffectiveLayout to handle the case where the parent node is
        // itself docked (no direct layout) - we walk up the chain to find
        // the first ancestor with a layout.
        const effectiveLayout = getEffectiveLayout(
          parentNode,
          currentState.nodeLayouts,
        );
        if (effectiveLayout !== undefined && dockedChildren.length > 0) {
          const undockLayouts = calculateUndockLayouts(
            dockedChildren,
            effectiveLayout,
          );
          for (const [nodeId, layout] of undockLayouts) {
            updatedLayouts.set(nodeId, layout);
          }
        }

        return {
          ...currentState,
          rootNodes: [...currentState.rootNodes, newNode],
          nodeLayouts: updatedLayouts,
          selectedNodes: new Set([newNode.nodeId]),
        };
      });
    }

    return newNode;
  }

  console.warn(
    `Cannot add operation node: unknown type '${derivedNodeId}' for source node ${parentNode.nodeId}`,
  );
  return undefined;
}

export async function addSourceNode(
  deps: NodeCrudDeps,
  state: ExplorePageState,
  id: string,
): Promise<void> {
  const descriptor = nodeRegistry.get(id);
  if (!descriptor) {
    console.warn(`Cannot add source node: unknown node type '${id}'`);
    return;
  }

  let initialState: PreCreateState | PreCreateState[] | null = {};

  if (descriptor.preCreate) {
    initialState = await descriptor.preCreate({sqlModules: deps.sqlModules});
  }

  // User cancelled the preCreate dialog
  if (initialState === null) {
    return;
  }

  // Handle both single node and multi-node creation
  const statesToCreate = Array.isArray(initialState)
    ? initialState
    : [initialState];

  const newNodes: QueryNode[] = [];
  for (const stateItem of statesToCreate) {
    try {
      const newNode = descriptor.factory(
        {
          ...stateItem,
          trace: deps.trace,
          sqlModules: deps.sqlModules,
        } as QueryNodeState,
        {allNodes: state.rootNodes},
      );
      newNodes.push(newNode);
    } catch (error) {
      console.error('Failed to create node:', error);
      // Continue creating other nodes even if one fails
    }
  }

  // If no nodes were successfully created, return early
  // (errors were already logged in the try-catch above)
  if (newNodes.length === 0) {
    console.warn('No nodes were created from the preCreate result');
    return;
  }

  const lastNode = newNodes[newNodes.length - 1];
  deps.onStateUpdate((currentState) => ({
    ...currentState,
    rootNodes: [...currentState.rootNodes, ...newNodes],
    selectedNodes: new Set([lastNode.nodeId]),
  }));
}

export async function addAndConnectTable(
  deps: NodeCrudDeps,
  state: ExplorePageState,
  tableName: string,
  targetNode: QueryNode,
  portIndex: number,
): Promise<void> {
  // Get the table descriptor
  const descriptor = nodeRegistry.get('table');
  if (!descriptor) {
    console.warn("Cannot add table: 'table' node type not found in registry");
    return;
  }

  // Find the table in SQL modules
  const sqlTable = deps.sqlModules
    .listTables()
    .find((t) => t.name === tableName);
  if (!sqlTable) {
    console.warn(`Table ${tableName} not found in SQL modules`);
    return;
  }

  // Create the table node with the specific table (bypass the modal)
  const newNode = descriptor.factory(
    {
      sqlTable,
      sqlModules: deps.sqlModules,
      trace: deps.trace,
    },
    {allNodes: state.rootNodes},
  );

  // Add connection from the new table node to the target node
  addConnection(newNode, targetNode, portIndex);

  // Add the new node to root nodes
  deps.onStateUpdate((currentState) => ({
    ...currentState,
    rootNodes: [...currentState.rootNodes, newNode],
  }));
}

export async function insertNodeAtPort(
  deps: NodeCrudDeps,
  state: ExplorePageState,
  targetNode: QueryNode,
  portIndex: number,
  descriptorKey: string,
): Promise<void> {
  const descriptor = nodeRegistry.get(descriptorKey);
  if (!descriptor) {
    console.warn(
      `Cannot insert ${descriptorKey} node: '${descriptorKey}' not found in registry`,
    );
    return;
  }

  const inputNode = getInputNodeAtPort(targetNode, portIndex);
  if (!inputNode) {
    console.warn(`No input node found at port ${portIndex}`);
    return;
  }

  const newNode = descriptor.factory(
    {
      sqlModules: deps.sqlModules,
      trace: deps.trace,
    },
    {allNodes: state.rootNodes},
  );

  removeConnection(inputNode, targetNode);
  addConnection(inputNode, newNode);
  addConnection(newNode, targetNode, portIndex);

  deps.onStateUpdate((currentState) => ({
    ...currentState,
    rootNodes: [...currentState.rootNodes, newNode],
    selectedNodes: new Set([newNode.nodeId]),
  }));
}

export async function deleteNode(
  deps: NodeCrudDeps,
  state: ExplorePageState,
  node: QueryNode,
): Promise<void> {
  // STEP 1: Clean up resources (SQL tables, JS subscriptions, etc.)
  if (deps.cleanupManager !== undefined) {
    try {
      await deps.cleanupManager.cleanupNode(node);
    } catch (error) {
      // Log error but continue with deletion
      console.error('Failed to cleanup node resources:', error);
    }
  }

  // STEP 2: Capture graph structure BEFORE modification
  // We need to capture this info before removeConnection() clears the references
  const primaryParent = getPrimaryParent(node);
  const childConnections = captureAllChildConnections(node);
  const allInputs = getAllInputNodes(node); // Capture ALL parents (primary + secondary)

  // STEP 3: Remove the node from the graph
  disconnectNodeFromGraph(node);

  // STEP 4: Reconnect primary parent to children (if exists)
  // This bypasses the deleted node, maintaining data flow for PRIMARY connections only.
  //
  // IMPORTANT RULES:
  // 1. Only reconnect if deleted node fed child's PRIMARY input (portIndex === undefined)
  // 2. Secondary connections are specific to the deleted node - DROP them, don't reconnect
  // 3. Skip reconnection if parent is already connected to avoid duplicates
  // 4. Transfer deleted node's layout to docked children so they can render at same position
  const reconnectedChildren: QueryNode[] = [];
  const updatedNodeLayouts = new Map(state.nodeLayouts);
  const deletedNodeLayout = state.nodeLayouts.get(node.nodeId);

  if (primaryParent !== undefined) {
    let layoutOffsetCount = 0;
    for (const {child, portIndex} of childConnections) {
      // If deleted node fed child's secondary input, DROP the connection
      // Secondary inputs are specific to the deleted node (e.g., intervals for FilterDuring)
      if (portIndex !== undefined) {
        continue; // Don't reconnect secondary connections
      }

      // Check if parent is already connected to this child
      if (primaryParent.nextNodes.includes(child)) {
        continue; // Already connected - don't create duplicates
      }

      // Reconnect: maintain primary data flow (A → B → C becomes A → C)
      addConnection(primaryParent, child, portIndex);
      reconnectedChildren.push(child);

      // If child was docked (no layout) and deleted node had a layout,
      // transfer the layout to the child so it renders at the same position
      // For multiple children, offset their positions to avoid overlapping
      const childHasNoLayout = !state.nodeLayouts.has(child.nodeId);
      if (childHasNoLayout && deletedNodeLayout !== undefined) {
        const offsetX = layoutOffsetCount * 30; // Offset each child by 30px
        const offsetY = layoutOffsetCount * 30;
        updatedNodeLayouts.set(child.nodeId, {
          x: deletedNodeLayout.x + offsetX,
          y: deletedNodeLayout.y + offsetY,
        });
        layoutOffsetCount++;
      }
    }
  }

  // STEP 4b: Check if reconnected children can actually be rendered
  // A child becomes "unrenderable" if:
  // - It was reconnected to a parent
  // - It has no layout (was docked to deleted node)
  // - Parent has multiple children (can't render as docked anymore)
  const unrenderableChildren: QueryNode[] = [];
  if (primaryParent !== undefined && reconnectedChildren.length > 0) {
    const parentHasMultipleChildren = primaryParent.nextNodes.length > 1;
    for (const child of reconnectedChildren) {
      // Check the UPDATED layouts, not the old state
      const childHasNoLayout = !updatedNodeLayouts.has(child.nodeId);
      // If child has no layout and parent has multiple children,
      // the child can't be rendered (not as docked, not as root)
      if (childHasNoLayout && parentHasMultipleChildren) {
        unrenderableChildren.push(child);
      }
    }
  }

  // STEP 5: Update root nodes list
  // Use a Set to prevent duplicate root nodes
  const newRootNodesSet = new Set(state.rootNodes.filter((n) => n !== node));

  // Add orphaned children to root nodes so they remain visible
  // Children are orphaned ONLY if:
  // 1. There was no primary parent to reconnect them to, AND
  // 2. They were connected via PRIMARY input (not secondary)
  // Children connected via secondary input still have their own primary parent!
  if (primaryParent === undefined && childConnections.length > 0) {
    // Only children connected via primary input are truly orphaned
    const orphanedChildren = childConnections
      .filter((c) => c.portIndex === undefined) // Primary input only
      .map((c) => c.child);

    for (const child of orphanedChildren) {
      newRootNodesSet.add(child);
    }

    // Transfer deleted node's layout to orphaned children so they appear at same position
    // For multiple children, offset their positions to avoid overlapping
    if (deletedNodeLayout !== undefined) {
      let layoutOffsetCount = 0;
      for (const child of orphanedChildren) {
        const childHasNoLayout = !updatedNodeLayouts.has(child.nodeId);
        if (childHasNoLayout) {
          const offsetX = layoutOffsetCount * 30; // Offset each child by 30px
          const offsetY = layoutOffsetCount * 30;
          updatedNodeLayouts.set(child.nodeId, {
            x: deletedNodeLayout.x + offsetX,
            y: deletedNodeLayout.y + offsetY,
          });
          layoutOffsetCount++;
        }
      }
    }
  }

  // Add unrenderable children to root nodes so they become visible
  // These are children that were reconnected but can't be rendered as docked
  for (const child of unrenderableChildren) {
    newRootNodesSet.add(child);
  }

  // STEP 5b: Promote orphaned input providers to root nodes
  // Simple rule: If a node was NOT a root node, and we deleted the node that
  // consumed it, then it should become a root node.
  const orphanedInputs: QueryNode[] = [];
  for (const inputNode of allInputs) {
    // Check if this input node becomes orphaned:
    // 1. It was NOT originally a root node
    // 2. After deletion, it has no consumers (nextNodes is empty)
    const wasNotRoot = !state.rootNodes.includes(inputNode);
    const hasNoConsumers = inputNode.nextNodes.length === 0;

    if (wasNotRoot && hasNoConsumers) {
      orphanedInputs.push(inputNode);
    }
  }

  for (const inputNode of orphanedInputs) {
    newRootNodesSet.add(inputNode);
  }

  const newRootNodes = Array.from(newRootNodesSet);

  // STEP 5c: Remove the deleted node's layout from the map
  // Now that we've transferred the layout to children/orphans, clean it up
  updatedNodeLayouts.delete(node.nodeId);

  // STEP 6: Trigger validation on affected children
  // Children need to re-validate because their inputs have changed
  // (either reconnected to a different parent or lost their parent entirely)
  for (const {child} of childConnections) {
    child.onPrevNodesUpdated?.();
  }

  // Also notify orphaned input providers that their consumers changed
  for (const inputNode of orphanedInputs) {
    notifyNextNodes(inputNode);
  }

  // STEP 7: Commit state changes
  deps.onStateUpdate((currentState) => {
    // Update selection based on current state (not stale state)
    // This is important for multi-node deletion where state changes between deletions
    const newSelectedNodes = new Set(currentState.selectedNodes);
    newSelectedNodes.delete(node.nodeId);

    return {
      ...currentState,
      rootNodes: newRootNodes,
      selectedNodes: newSelectedNodes,
      nodeLayouts: updatedNodeLayouts,
    };
  });
}

// Delete all currently selected nodes.
// Batches all deletions into a single state update to create one undo point.
export async function deleteSelectedNodes(
  deps: NodeCrudDeps,
  state: ExplorePageState,
): Promise<void> {
  const selectedNodeIds = new Set(state.selectedNodes);

  if (selectedNodeIds.size === 0) {
    return;
  }

  // Get all nodes to delete
  const allNodes = getAllNodes(state.rootNodes);
  const nodesToDelete = allNodes.filter((n) => selectedNodeIds.has(n.nodeId));

  if (nodesToDelete.length === 0) {
    return;
  }

  // STEP 1: Clean up resources for all nodes (async operations)
  if (deps.cleanupManager !== undefined) {
    for (const node of nodesToDelete) {
      try {
        await deps.cleanupManager.cleanupNode(node);
      } catch (error) {
        console.error('Failed to cleanup node resources:', error);
      }
    }
  }

  // STEP 2: Capture graph info and perform all deletions in a single state update
  deps.onStateUpdate((currentState) => {
    const nodesToDeleteSet = new Set(nodesToDelete);
    const updatedNodeLayouts = new Map(currentState.nodeLayouts);
    const newRootNodesSet = new Set(currentState.rootNodes);
    const affectedChildren: QueryNode[] = [];
    const orphanedInputs: QueryNode[] = [];

    // Process each node deletion
    for (const node of nodesToDelete) {
      // Capture info before disconnection
      const primaryParent = getPrimaryParent(node);
      const childConnections = captureAllChildConnections(node);
      const allInputs = getAllInputNodes(node);

      // Disconnect from graph
      disconnectNodeFromGraph(node);

      // Remove from root nodes
      newRootNodesSet.delete(node);

      // Remove layout
      const deletedNodeLayout = updatedNodeLayouts.get(node.nodeId);
      updatedNodeLayouts.delete(node.nodeId);

      // Reconnect primary parent to children (if parent is not also being deleted)
      if (primaryParent !== undefined && !nodesToDeleteSet.has(primaryParent)) {
        let layoutOffsetCount = 0;
        for (const {child, portIndex} of childConnections) {
          // Skip if child is also being deleted
          if (nodesToDeleteSet.has(child)) {
            continue;
          }

          // Only reconnect primary connections
          if (portIndex === undefined) {
            if (!primaryParent.nextNodes.includes(child)) {
              addConnection(primaryParent, child, portIndex);
              affectedChildren.push(child);

              // Transfer layout if child was docked
              const childHasNoLayout = !updatedNodeLayouts.has(child.nodeId);
              if (childHasNoLayout && deletedNodeLayout !== undefined) {
                const offsetX = layoutOffsetCount * 30;
                const offsetY = layoutOffsetCount * 30;
                updatedNodeLayouts.set(child.nodeId, {
                  x: deletedNodeLayout.x + offsetX,
                  y: deletedNodeLayout.y + offsetY,
                });
                layoutOffsetCount++;
              }
            }
          }
        }
      }

      // Handle orphaned children (no parent or parent was deleted)
      if (primaryParent === undefined || nodesToDeleteSet.has(primaryParent)) {
        let layoutOffsetCount = 0;
        for (const {child, portIndex} of childConnections) {
          // Skip if child is also being deleted
          if (nodesToDeleteSet.has(child)) {
            continue;
          }

          // Only orphan primary connections
          if (portIndex === undefined) {
            newRootNodesSet.add(child);
            affectedChildren.push(child);

            // Transfer layout
            const childHasNoLayout = !updatedNodeLayouts.has(child.nodeId);
            if (childHasNoLayout && deletedNodeLayout !== undefined) {
              const offsetX = layoutOffsetCount * 30;
              const offsetY = layoutOffsetCount * 30;
              updatedNodeLayouts.set(child.nodeId, {
                x: deletedNodeLayout.x + offsetX,
                y: deletedNodeLayout.y + offsetY,
              });
              layoutOffsetCount++;
            }
          }
        }
      }

      // Handle orphaned input providers
      for (const inputNode of allInputs) {
        // Skip if input is also being deleted
        if (nodesToDeleteSet.has(inputNode)) {
          continue;
        }

        const wasNotRoot = !currentState.rootNodes.includes(inputNode);
        const hasNoConsumers = inputNode.nextNodes.length === 0;

        if (wasNotRoot && hasNoConsumers) {
          newRootNodesSet.add(inputNode);
          orphanedInputs.push(inputNode);
        }
      }
    }

    // Trigger validation on affected nodes
    for (const child of affectedChildren) {
      child.onPrevNodesUpdated?.();
    }
    for (const inputNode of orphanedInputs) {
      notifyNextNodes(inputNode);
    }

    // Clear selection
    return {
      ...currentState,
      rootNodes: Array.from(newRootNodesSet),
      selectedNodes: new Set<string>(),
      nodeLayouts: updatedNodeLayouts,
    };
  });
}

export async function clearAllNodes(
  deps: NodeCrudDeps,
  state: ExplorePageState,
): Promise<void> {
  await cleanupExistingNodes(
    deps.cleanupManager,
    deps.initializedNodes,
    state.rootNodes,
  );

  deps.onStateUpdate((currentState) => ({
    ...currentState,
    rootNodes: [],
    selectedNodes: new Set(),
    nodeLayouts: new Map(),
    labels: [],
  }));
}

export function duplicateNode(
  onStateUpdate: (
    update: (currentState: ExplorePageState) => ExplorePageState,
  ) => void,
  node: QueryNode,
): void {
  onStateUpdate((currentState) => ({
    ...currentState,
    rootNodes: [...currentState.rootNodes, node.clone()],
  }));
}

export function removeNodeConnection(
  state: ExplorePageState,
  onStateUpdate: (
    update: (currentState: ExplorePageState) => ExplorePageState,
  ) => void,
  fromNode: QueryNode,
  toNode: QueryNode,
  isSecondaryInput: boolean,
): void {
  // NOTE: The basic connection removal is already handled by graph.ts
  // This callback handles higher-level logic like reconnection and state updates

  // Only reconnect fromNode to toNode's children when removing a PRIMARY input.
  // When removing a SECONDARY input, we should NOT reconnect - the secondary
  // input node is just an auxiliary input (like intervals for FilterDuring)
  // and should not be connected to the children of the node it was feeding into.
  const shouldReconnect =
    !isSecondaryInput &&
    fromNode.nextNodes.length === 0 &&
    toNode.nextNodes.length > 0;

  if (shouldReconnect) {
    // Reconnect fromNode to all of toNode's children (bypass toNode)
    for (const child of toNode.nextNodes) {
      addConnection(fromNode, child);
    }
  }

  // Handle state updates based on node type
  if ('primaryInput' in toNode && toNode.primaryInput === undefined) {
    // toNode is a ModificationNode that's now orphaned
    // Add it to rootNodes so it remains visible (but invalid)
    const newRootNodes = state.rootNodes.includes(toNode)
      ? state.rootNodes
      : [...state.rootNodes, toNode];

    onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: newRootNodes,
    }));
  } else if ('secondaryInputs' in toNode) {
    // toNode is a MultiSourceNode - just trigger a state update
    onStateUpdate((currentState) => ({...currentState}));
  }
}
