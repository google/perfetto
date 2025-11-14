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
import {Trace} from '../../public/trace';
import {SqlModules} from '../../plugins/dev.perfetto.SqlModules/sql_modules';
import {
  AddColumnsNode,
  AddColumnsNodeState,
} from './query_builder/nodes/dev/add_columns_node';
import {
  LimitAndOffsetNode,
  LimitAndOffsetNodeState,
} from './query_builder/nodes/dev/limit_and_offset_node';
import {SortNode, SortNodeState} from './query_builder/nodes/dev/sort_node';

type SerializedNodeState =
  | TableSourceSerializedState
  | SlicesSourceSerializedState
  | SqlSourceSerializedState
  | AggregationSerializedState
  | ModifyColumnsSerializedState
  | IntervalIntersectSerializedState
  | AddColumnsNodeState
  | LimitAndOffsetNodeState
  | SortNodeState;

// Interfaces for the serialized JSON structure
export interface SerializedNode {
  nodeId: string;
  type: NodeType;
  state: SerializedNodeState; // This will hold the serializable state of the node
  nextNodes: string[];
  prevNode?: string;
  prevNodes?: string[];
}

export interface SerializedGraph {
  nodes: SerializedNode[];
  rootNodeIds: string[];
  selectedNodeId?: string;
  nodeLayouts: {[key: string]: {x: number; y: number}};
}

function serializeNode(node: QueryNode): SerializedNode {
  if (typeof node.serializeState !== 'function') {
    throw new Error(`Node type ${node.type} is not serializable.`);
  }

  const state = node.serializeState() as SerializedNodeState;

  const serialized: SerializedNode = {
    nodeId: node.nodeId,
    type: node.type,
    state: state,
    nextNodes: node.nextNodes.map((n: QueryNode) => n.nodeId),
  };

  if ('prevNode' in node && node.prevNode) {
    serialized.prevNode = node.prevNode.nodeId;
  } else if ('prevNodes' in node) {
    serialized.prevNodes = node.prevNodes
      .filter((n) => n !== undefined)
      .map((n) => n!.nodeId);
  }

  return serialized;
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

  const replacer = (key: string, value: unknown) => {
    if (key === 'prevNodes' || key === 'prevNode' || key === '_trace') {
      return undefined;
    }
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
      return new ModifyColumnsNode(
        ModifyColumnsNode.deserializeState(
          state as ModifyColumnsSerializedState,
        ),
      );
    case NodeType.kAddColumns:
      return new AddColumnsNode(
        AddColumnsNode.deserializeState(state as AddColumnsNodeState),
      );
    case NodeType.kLimitAndOffset:
      return new LimitAndOffsetNode(
        LimitAndOffsetNode.deserializeState(state as LimitAndOffsetNodeState),
      );
    case NodeType.kSort:
      return new SortNode(SortNode.deserializeState(state as SortNodeState));
    case NodeType.kIntervalIntersect:
      const nodeState: IntervalIntersectNodeState = {
        ...(state as IntervalIntersectSerializedState),
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
      serializedNode.prevNode === undefined &&
      serializedNode.prevNodes === undefined
    ) {
      for (const nextNode of node.nextNodes) {
        if ('prevNode' in nextNode) {
          (nextNode as {prevNode: QueryNode}).prevNode = node;
        } else if ('prevNodes' in nextNode) {
          nextNode.prevNodes.push(node);
        }
      }
    }

    if (serializedNode.prevNode) {
      if ('prevNode' in node) {
        const prevNode = nodes.get(serializedNode.prevNode);
        if (prevNode) {
          (node as {prevNode: QueryNode}).prevNode = prevNode;
        }
      }
    }

    if (serializedNode.prevNodes) {
      if ('prevNodes' in node) {
        for (const id of serializedNode.prevNodes) {
          const prevNode = nodes.get(id);
          if (prevNode) {
            node.prevNodes.push(prevNode);
          }
        }
      } else if ('prevNode' in node && serializedNode.prevNodes.length > 0) {
        // Backwards compatibility
        const prevNode = nodes.get(serializedNode.prevNodes[0]);
        if (prevNode) {
          (node as {prevNode: QueryNode}).prevNode = prevNode;
        }
      }
    }
    if (serializedNode.type === NodeType.kIntervalIntersect) {
      const intervalNode = node as IntervalIntersectNode;
      if (intervalNode.prevNodes.length > 0) {
        const deserializedState = IntervalIntersectNode.deserializeState(
          nodes,
          serializedNode.state as IntervalIntersectSerializedState,
          intervalNode.prevNodes[0],
        );
        intervalNode.prevNodes.length = 0;
        intervalNode.prevNodes.push(...deserializedState.prevNodes);
      }
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
