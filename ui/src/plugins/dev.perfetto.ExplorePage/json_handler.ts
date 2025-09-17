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
import {QueryNode, NodeType} from './query_node';
import {
  TableSourceNode,
  TableSourceSerializedState,
} from './query_builder/nodes/sources/table_source';
import {
  SlicesSourceNode,
  SlicesSourceSerializedState,
} from './query_builder/nodes/sources/slices_source';
import {
  SqlSourceNode,
  SqlSourceSerializedState,
} from './query_builder/nodes/sources/sql_source';
import {
  AggregationNode,
  AggregationSerializedState,
} from './query_builder/nodes/aggregation_node';
import {
  ModifyColumnsNode,
  ModifyColumnsSerializedState,
} from './query_builder/nodes/modify_columns_node';
import {
  IntervalIntersectNode,
  IntervalIntersectNodeState,
  IntervalIntersectSerializedState,
} from './query_builder/nodes/interval_intersect_node';
import {NodeBoxLayout} from './query_builder/node_box';
import {Trace} from '../../public/trace';
import {SqlModules} from '../../plugins/dev.perfetto.SqlModules/sql_modules';

type SerializedNodeState =
  | TableSourceSerializedState
  | SlicesSourceSerializedState
  | SqlSourceSerializedState
  | AggregationSerializedState
  | ModifyColumnsSerializedState
  | IntervalIntersectSerializedState;

// Interfaces for the serialized JSON structure
export interface SerializedNode {
  nodeId: string;
  type: NodeType;
  state: SerializedNodeState; // This will hold the serializable state of the node
  nextNodes: string[];
  prevNodes: string[];
}

export interface SerializedGraph {
  nodes: SerializedNode[];
  rootNodeIds: string[];
  selectedNodeId?: string;
  nodeLayouts: {[key: string]: NodeBoxLayout};
}

function serializeNode(node: QueryNode): SerializedNode {
  if (typeof node.serializeState !== 'function') {
    throw new Error(`Node type ${node.type} is not serializable.`);
  }

  const state = node.serializeState() as SerializedNodeState;

  return {
    nodeId: node.nodeId,
    type: node.type,
    state: state,
    nextNodes: node.nextNodes.map((n: QueryNode) => n.nodeId),
    prevNodes: node.prevNodes
      ? node.prevNodes.map((n: QueryNode) => n.nodeId)
      : [],
  };
}

export function serializeState(state: ExplorePageState): string {
  const allNodes = new Map<string, QueryNode>();
  const queue = [...state.rootNodes];
  while (queue.length > 0) {
    const node = queue.shift()!;
    if (!allNodes.has(node.nodeId)) {
      allNodes.set(node.nodeId, node);
      queue.push(...node.nextNodes);
    }
  }

  const serializedNodes = Array.from(allNodes.values()).map(serializeNode);

  const serializedGraph: SerializedGraph = {
    nodes: serializedNodes,
    rootNodeIds: state.rootNodes.map((n) => n.nodeId),
    selectedNodeId: state.selectedNode?.nodeId,
    nodeLayouts: Object.fromEntries(state.nodeLayouts),
  };

  const replacer = (_key: string, value: unknown) =>
    typeof value === 'bigint' ? value.toString() : value;

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
  const {state} = serializedNode;
  switch (serializedNode.type) {
    case NodeType.kTable:
      return new TableSourceNode(
        TableSourceNode.deserializeState(
          trace,
          sqlModules,
          state as TableSourceSerializedState,
        ),
      );
    case NodeType.kSimpleSlices:
      return new SlicesSourceNode(state as SlicesSourceSerializedState);
    case NodeType.kSqlSource:
      return new SqlSourceNode({
        ...(state as SqlSourceSerializedState),
        trace,
      });
    case NodeType.kAggregation:
      return new AggregationNode(
        AggregationNode.deserializeState(state as AggregationSerializedState),
      );
    case NodeType.kModifyColumns:
      return new ModifyColumnsNode({
        ...(state as ModifyColumnsSerializedState),
        prevNodes: [],
      });
    case NodeType.kIntervalIntersect:
      const nodeState: IntervalIntersectNodeState = {
        ...(state as IntervalIntersectSerializedState),
        intervalNodes: [],
        prevNodes: [],
        allNodes: [],
      };
      return new IntervalIntersectNode(nodeState);
    default:
      throw new Error(`Unknown node type: ${serializedNode.type}`);
  }
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
    !Array.isArray(serializedGraph.rootNodeIds) ||
    serializedGraph.nodeLayouts == null ||
    typeof serializedGraph.nodeLayouts !== 'object'
  ) {
    throw new Error(
      'Invalid file format. The selected file is not a valid Perfetto graph.',
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

  // Second pass: connect nodes
  for (const serializedNode of serializedGraph.nodes) {
    const node = nodes.get(serializedNode.nodeId);
    if (!node) {
      throw new Error(
        `Graph is corrupted. Node with ID "${serializedNode.nodeId}" was serialized but not instantiated.`,
      );
    }
    node.nextNodes = serializedNode.nextNodes.map((id) => {
      const nextNode = nodes.get(id);
      if (nextNode == null) {
        throw new Error(`Graph is corrupted. Node "${id}" not found.`);
      }
      return nextNode;
    });

    // Backwards compatibility: if prevNodes is not in the JSON, infer it.
    if (
      serializedNode.prevNodes !== undefined &&
      serializedNode.prevNodes.length > 0
    ) {
      node.prevNodes = serializedNode.prevNodes.map((id) => {
        const prevNode = nodes.get(id);
        if (prevNode == null) {
          throw new Error(`Graph is corrupted. Node "${id}" not found.`);
        }
        return prevNode;
      });
    } else {
      for (const nextNode of node.nextNodes) {
        if (nextNode.prevNodes == null) {
          nextNode.prevNodes = [];
        }
        nextNode.prevNodes.push(node);
      }
    }
    if (serializedNode.type === NodeType.kIntervalIntersect) {
      (node as IntervalIntersectNode).state.intervalNodes =
        IntervalIntersectNode.deserializeState(
          nodes,
          serializedNode.state as IntervalIntersectSerializedState,
        ).intervalNodes;
    }
  }

  // Third pass: resolve columns
  for (const node of nodes.values()) {
    if (node.type === NodeType.kAggregation) {
      (node as AggregationNode).resolveColumns();
    }
  }

  const rootNodes = serializedGraph.rootNodeIds.map((id) => {
    const rootNode = nodes.get(id)!;
    if (rootNode == null) {
      throw new Error(`Graph is corrupted. Root node "${id}" not found.`);
    }
    return rootNode;
  });
  const selectedNode = serializedGraph.selectedNodeId
    ? nodes.get(serializedGraph.selectedNodeId)
    : undefined;

  return {
    rootNodes,
    selectedNode,
    nodeLayouts: new Map(Object.entries(serializedGraph.nodeLayouts)),
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
