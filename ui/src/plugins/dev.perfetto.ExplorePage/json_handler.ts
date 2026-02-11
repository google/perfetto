// Copyright (C) 2024 The Android Open Source Project
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

import {ExplorePageState} from './explore_page';
import {QueryNode, NodeType, ensureCounterAbove} from './query_node';
import {getAllNodes as getAllNodesUtil} from './query_builder/graph_utils';
import {Trace} from '../../public/trace';
import {SqlModules} from '../../plugins/dev.perfetto.SqlModules/sql_modules';
import {nodeRegistry} from './query_builder/node_registry';

// Interfaces for the serialized JSON structure
export interface SerializedNode {
  nodeId: string;
  type: NodeType;
  state: object;
  nextNodes: string[];
  // Input node IDs (for multi-source nodes like Union, Merge, IntervalIntersect)
  inputNodeIds?: string[];
}

export interface SerializedGraph {
  nodes: SerializedNode[];
  rootNodeIds: string[];
  selectedNodeId?: string;
  nodeLayouts?: {[key: string]: {x: number; y: number}};
  labels?: Array<{
    id: string;
    x: number;
    y: number;
    width: number;
    text: string;
  }>;
  isExplorerCollapsed?: boolean;
  sidebarWidth?: number;
}

function serializeNode(node: QueryNode): SerializedNode {
  if (typeof node.serializeState !== 'function') {
    throw new Error(`Node type ${node.type} is not serializable.`);
  }

  return {
    nodeId: node.nodeId,
    type: node.type,
    state: node.serializeState(),
    nextNodes: node.nextNodes.map((n: QueryNode) => n.nodeId),
  };
}

interface LabelData {
  id: string;
  x: number;
  y: number;
  width: number;
  text: string;
}

/**
 * Normalizes layout coordinates so that the top-left corner is at (minX, minY).
 * This ensures consistent positioning when loading/exporting graphs.
 */
function normalizeLayoutCoordinates(
  nodeLayouts: Map<string, {x: number; y: number}>,
  labels: LabelData[],
): {
  nodeLayouts: Map<string, {x: number; y: number}>;
  labels: LabelData[];
} {
  // Collect all x and y coordinates from node layouts and labels
  const xCoords: number[] = [];
  const yCoords: number[] = [];

  for (const layout of nodeLayouts.values()) {
    xCoords.push(layout.x);
    yCoords.push(layout.y);
  }

  for (const label of labels) {
    xCoords.push(label.x);
    yCoords.push(label.y);
  }

  // If there are no coordinates, return as-is
  if (xCoords.length === 0) {
    return {nodeLayouts, labels};
  }

  const minX = Math.min(...xCoords);
  const minY = Math.min(...yCoords);

  // If already normalized (minX and minY are 0), return as-is
  if (minX === 0 && minY === 0) {
    return {nodeLayouts, labels};
  }

  // Create new normalized layouts
  const normalizedLayouts = new Map<string, {x: number; y: number}>();
  for (const [nodeId, layout] of nodeLayouts) {
    normalizedLayouts.set(nodeId, {
      x: layout.x - minX,
      y: layout.y - minY,
    });
  }

  // Normalize labels
  const normalizedLabels = labels.map((label) => ({
    ...label,
    x: label.x - minX,
    y: label.y - minY,
  }));

  return {nodeLayouts: normalizedLayouts, labels: normalizedLabels};
}

export function serializeState(state: ExplorePageState): string {
  // Use utility function to get all nodes (bidirectional traversal)
  const allNodesArray = getAllNodesUtil(state.rootNodes);
  const allNodes = new Map<string, QueryNode>();
  for (const node of allNodesArray) {
    allNodes.set(node.nodeId, node);
  }

  const serializedNodes = Array.from(allNodes.values()).map(serializeNode);

  // Normalize coordinates so top-left corner is at (0, 0) when exporting
  const normalized = normalizeLayoutCoordinates(
    state.nodeLayouts,
    state.labels,
  );

  // For backward compatibility, save the first selected node ID if any nodes are selected
  const firstSelectedNodeId =
    state.selectedNodes.size > 0
      ? state.selectedNodes.values().next().value
      : undefined;

  const serializedGraph: SerializedGraph = {
    nodes: serializedNodes,
    rootNodeIds: state.rootNodes.map((n) => n.nodeId),
    selectedNodeId: firstSelectedNodeId,
    nodeLayouts: Object.fromEntries(normalized.nodeLayouts),
    labels: normalized.labels,
    isExplorerCollapsed: state.isExplorerCollapsed,
    sidebarWidth: state.sidebarWidth,
  };

  const replacer = (key: string, value: unknown) => {
    // Only strip _trace to avoid including large trace objects
    if (key === '_trace') {
      return undefined;
    }
    // Connection info is stored in node-specific state (primaryInputId, inputNodeIds, etc.)
    // so we don't need to filter them here
    return typeof value === 'bigint' ? value.toString() : value;
  };

  return JSON.stringify(serializedGraph, replacer, 2);
}

export function exportStateAsJson(state: ExplorePageState, trace: Trace): void {
  const json = serializeState(state);
  const blob = new Blob([json], {type: 'application/json'});
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;

  const traceName = trace.traceInfo.traceTitle.replace(
    /[^a-zA-Z0-9._-]+/g,
    '_',
  );
  const date = new Date().toISOString().slice(0, 10);
  a.download = `${traceName}-graph-${date}.json`;

  a.click();
  URL.revokeObjectURL(url);
}

function createNodeInstance(
  serializedNode: SerializedNode,
  trace: Trace,
  sqlModules: SqlModules,
): QueryNode {
  const descriptor = nodeRegistry.getByNodeType(serializedNode.type);
  if (!descriptor) {
    throw new Error(`Unknown node type: ${serializedNode.type}`);
  }
  return descriptor.deserialize(serializedNode.state, trace, sqlModules);
}

export function deserializeState(
  json: string,
  trace: Trace,
  sqlModules: SqlModules,
): ExplorePageState {
  const serializedGraph: SerializedGraph = JSON.parse(json);

  // Basic validation to ensure the file is a Perfetto graph export.
  if (
    serializedGraph == null ||
    typeof serializedGraph !== 'object' ||
    !Array.isArray(serializedGraph.nodes) ||
    !Array.isArray(serializedGraph.rootNodeIds)
  ) {
    throw new Error(
      'Invalid file format. The selected file is not a valid Perfetto graph.',
    );
  }

  // Validate nodeLayouts if present
  if (
    serializedGraph.nodeLayouts != null &&
    typeof serializedGraph.nodeLayouts !== 'object'
  ) {
    throw new Error(
      'Invalid file format. nodeLayouts must be an object if provided.',
    );
  }

  const nodes = new Map<string, QueryNode>();
  // First pass: create all node instances
  for (const serializedNode of serializedGraph.nodes) {
    const node = createNodeInstance(serializedNode, trace, sqlModules);
    // Overwrite the newly generated nodeId with the one from the file
    // to allow re-linking nodes correctly.
    (node as {nodeId: string}).nodeId = serializedNode.nodeId;
    nodes.set(serializedNode.nodeId, node);
  }

  // Ensure the global node counter is above all loaded IDs to prevent collisions
  ensureCounterAbove(serializedGraph.nodes.map((n) => n.nodeId));

  // Second pass: set forward links (nextNodes)
  for (const serializedNode of serializedGraph.nodes) {
    const node = nodes.get(serializedNode.nodeId);
    if (!node) {
      throw new Error(
        `Graph is corrupted. Node with ID "${serializedNode.nodeId}" was serialized but not instantiated.`,
      );
    }

    // Set forward links (nextNodes)
    node.nextNodes = serializedNode.nextNodes.map((id) => {
      const nextNode = nodes.get(id);
      if (nextNode == null) {
        throw new Error(`Graph is corrupted. Node "${id}" not found.`);
      }
      return nextNode;
    });
  }

  // Third pass: set backward connections using the node registry
  for (const serializedNode of serializedGraph.nodes) {
    const node = nodes.get(serializedNode.nodeId);
    if (!node) {
      throw new Error(
        `Graph is corrupted. Node "${serializedNode.nodeId}" not found.`,
      );
    }
    const descriptor = nodeRegistry.getByNodeType(serializedNode.type);
    if (!descriptor) {
      throw new Error(`Unknown node type: ${serializedNode.type}`);
    }

    // Restore primary input for nodes that have one
    const hasPrimary =
      descriptor.hasPrimaryInput ?? descriptor.type === 'modification';
    if (hasPrimary) {
      const serializedState = serializedNode.state as {
        primaryInputId?: string;
      };
      if (serializedState.primaryInputId) {
        const inputNode = nodes.get(serializedState.primaryInputId);
        if (inputNode) {
          node.primaryInput = inputNode;
        }
      }
    }

    // Node-specific connection deserialization
    descriptor.deserializeConnections?.(node, serializedNode.state, nodes);
  }

  // Fourth pass: post-deserialization (resolve internal references, then
  // update derived state). Two phases ensure that all nodes are resolved
  // before any derived state is computed.
  const descriptors = [...nodes.values()].map((node) => ({
    node,
    descriptor: nodeRegistry.getByNodeType(node.type),
  }));
  for (const {node, descriptor} of descriptors) {
    descriptor?.postDeserialize?.(node);
  }
  for (const {node, descriptor} of descriptors) {
    descriptor?.postDeserializeLate?.(node);
  }

  const rootNodes = serializedGraph.rootNodeIds.map((id) => {
    const rootNode = nodes.get(id)!;
    if (rootNode == null) {
      throw new Error(`Graph is corrupted. Root node "${id}" not found.`);
    }
    return rootNode;
  });
  // For backward compatibility, load selectedNodeId from saved state (if present)
  const selectedNode = serializedGraph.selectedNodeId
    ? nodes.get(serializedGraph.selectedNodeId)
    : undefined;

  // Use provided nodeLayouts if present, otherwise use empty map (will trigger auto-layout)
  let nodeLayouts =
    serializedGraph.nodeLayouts != null
      ? new Map(Object.entries(serializedGraph.nodeLayouts))
      : new Map<string, {x: number; y: number}>();

  // Normalize coordinates so top-left corner is at (minX, minY)
  let labels = serializedGraph.labels ?? [];
  const normalized = normalizeLayoutCoordinates(nodeLayouts, labels);
  nodeLayouts = normalized.nodeLayouts;
  labels = normalized.labels;

  return {
    rootNodes,
    selectedNodes: selectedNode ? new Set([selectedNode.nodeId]) : new Set(),
    nodeLayouts,
    labels,
    isExplorerCollapsed: serializedGraph.isExplorerCollapsed,
    sidebarWidth: serializedGraph.sidebarWidth,
  };
}

export function importStateFromJson(
  file: File,
  trace: Trace,
  sqlModules: SqlModules,
  onStateLoaded: (state: ExplorePageState) => void,
): void {
  const reader = new FileReader();
  reader.onload = (event) => {
    const json = event.target?.result as string;
    if (!json) {
      throw new Error('The selected file is empty or could not be read.');
    }
    const newState = deserializeState(json, trace, sqlModules);
    onStateLoaded(newState);
  };
  reader.readAsText(file);
}
