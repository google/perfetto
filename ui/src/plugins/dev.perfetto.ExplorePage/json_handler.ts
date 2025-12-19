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
import {QueryNode, NodeType, singleNodeOperation} from './query_node';
import {getAllNodes as getAllNodesUtil} from './query_builder/graph_utils';
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
  TimeRangeSourceNode,
  TimeRangeSourceSerializedState,
} from './query_builder/nodes/sources/timerange_source';
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
  IntervalIntersectSerializedState,
} from './query_builder/nodes/interval_intersect_node';
import {Trace} from '../../public/trace';
import {SqlModules} from '../../plugins/dev.perfetto.SqlModules/sql_modules';
import {
  AddColumnsNode,
  AddColumnsNodeState,
} from './query_builder/nodes/add_columns_node';
import {
  LimitAndOffsetNode,
  LimitAndOffsetNodeState,
} from './query_builder/nodes/limit_and_offset_node';
import {SortNode, SortNodeState} from './query_builder/nodes/sort_node';
import {FilterNode, FilterNodeState} from './query_builder/nodes/filter_node';
import {JoinNode, JoinSerializedState} from './query_builder/nodes/join_node';
import {
  CreateSlicesNode,
  CreateSlicesSerializedState,
} from './query_builder/nodes/create_slices_node';
import {
  UnionNode,
  UnionSerializedState,
} from './query_builder/nodes/union_node';
import {
  FilterDuringNode,
  FilterDuringNodeState,
} from './query_builder/nodes/filter_during_node';

type SerializedNodeState =
  | TableSourceSerializedState
  | SlicesSourceSerializedState
  | SqlSourceSerializedState
  | TimeRangeSourceSerializedState
  | AggregationSerializedState
  | ModifyColumnsSerializedState
  | IntervalIntersectSerializedState
  | AddColumnsNodeState
  | LimitAndOffsetNodeState
  | SortNodeState
  | FilterNodeState
  | JoinSerializedState
  | CreateSlicesSerializedState
  | UnionSerializedState
  | FilterDuringNodeState;

// Interfaces for the serialized JSON structure
export interface SerializedNode {
  nodeId: string;
  type: NodeType;
  state: SerializedNodeState; // This will hold the serializable state of the node
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

  const state = node.serializeState() as SerializedNodeState;

  const serialized: SerializedNode = {
    nodeId: node.nodeId,
    type: node.type,
    state: state,
    nextNodes: node.nextNodes.map((n: QueryNode) => n.nodeId),
  };

  // Connection information is stored in nextNodes and node-specific serializedState
  // Each node's serializeState() method handles its own input connections

  return serialized;
}

export function serializeState(state: ExplorePageState): string {
  // Use utility function to get all nodes (bidirectional traversal)
  const allNodesArray = getAllNodesUtil(state.rootNodes);
  const allNodes = new Map<string, QueryNode>();
  for (const node of allNodesArray) {
    allNodes.set(node.nodeId, node);
  }

  const serializedNodes = Array.from(allNodes.values()).map(serializeNode);

  const serializedGraph: SerializedGraph = {
    nodes: serializedNodes,
    rootNodeIds: state.rootNodes.map((n) => n.nodeId),
    selectedNodeId: state.selectedNode?.nodeId,
    nodeLayouts: Object.fromEntries(state.nodeLayouts),
    labels: state.labels,
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
      return new SlicesSourceNode({});
    case NodeType.kSqlSource:
      return new SqlSourceNode({
        ...(state as SqlSourceSerializedState),
        trace,
      });
    case NodeType.kTimeRangeSource:
      return new TimeRangeSourceNode(
        TimeRangeSourceNode.deserializeState(
          trace,
          state as TimeRangeSourceSerializedState,
        ),
      );
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
    case NodeType.kFilter:
      return new FilterNode(
        FilterNode.deserializeState(state as FilterNodeState),
      );
    case NodeType.kIntervalIntersect:
      return new IntervalIntersectNode(
        IntervalIntersectNode.deserializeState(
          state as IntervalIntersectSerializedState,
        ),
      );
    case NodeType.kJoin:
      return new JoinNode(
        JoinNode.deserializeState(state as JoinSerializedState),
      );
    case NodeType.kCreateSlices:
      return new CreateSlicesNode(
        CreateSlicesNode.deserializeState(state as CreateSlicesSerializedState),
      );
    case NodeType.kUnion:
      return new UnionNode(
        UnionNode.deserializeState(state as UnionSerializedState),
      );
    case NodeType.kFilterDuring:
      return new FilterDuringNode(
        FilterDuringNode.deserializeState(state as FilterDuringNodeState),
      );
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

  // Third pass: set backward connections using serialized state
  // For single-input operations, we use primaryInputId from state rather than inferring
  // from nextNodes. This is important for nodes like AddColumnsNode that have both
  // primaryInput AND secondaryInputs.
  for (const serializedNode of serializedGraph.nodes) {
    const node = nodes.get(serializedNode.nodeId)!;
    const serializedState = serializedNode.state as {primaryInputId?: string};

    // Set primaryInput for single-input operations using the serialized primaryInputId
    if (singleNodeOperation(node.type)) {
      if (serializedState.primaryInputId) {
        const inputNode = nodes.get(serializedState.primaryInputId);
        if (inputNode) {
          node.primaryInput = inputNode;
        }
      }
    }

    // Node-specific connection deserialization for multi-input operations
    if (serializedNode.type === NodeType.kIntervalIntersect) {
      const intervalNode = node as IntervalIntersectNode;
      const serializedState =
        serializedNode.state as IntervalIntersectSerializedState;
      const deserializedConnections =
        IntervalIntersectNode.deserializeConnections(nodes, serializedState);
      intervalNode.secondaryInputs.connections.clear();
      for (let i = 0; i < deserializedConnections.inputNodes.length; i++) {
        intervalNode.secondaryInputs.connections.set(
          i,
          deserializedConnections.inputNodes[i],
        );
      }
    }
    if (serializedNode.type === NodeType.kJoin) {
      const joinNode = node as JoinNode;
      const deserializedConnections = JoinNode.deserializeConnections(
        nodes,
        serializedNode.state as JoinSerializedState,
      );
      if (deserializedConnections.leftNode) {
        joinNode.secondaryInputs.connections.set(
          0,
          deserializedConnections.leftNode,
        );
      }
      if (deserializedConnections.rightNode) {
        joinNode.secondaryInputs.connections.set(
          1,
          deserializedConnections.rightNode,
        );
      }
    }
    if (serializedNode.type === NodeType.kCreateSlices) {
      const createSlicesNode = node as CreateSlicesNode;
      const deserializedConnections = CreateSlicesNode.deserializeConnections(
        nodes,
        serializedNode.state as CreateSlicesSerializedState,
      );
      if (deserializedConnections.startsNode) {
        createSlicesNode.secondaryInputs.connections.set(
          0,
          deserializedConnections.startsNode,
        );
      }
      if (deserializedConnections.endsNode) {
        createSlicesNode.secondaryInputs.connections.set(
          1,
          deserializedConnections.endsNode,
        );
      }
    }
    if (serializedNode.type === NodeType.kUnion) {
      const unionNode = node as UnionNode;
      const serializedState = serializedNode.state as UnionSerializedState;
      const deserializedConnections = UnionNode.deserializeConnections(
        nodes,
        serializedState,
      );
      unionNode.secondaryInputs.connections.clear();
      for (let i = 0; i < deserializedConnections.inputNodes.length; i++) {
        unionNode.secondaryInputs.connections.set(
          i,
          deserializedConnections.inputNodes[i],
        );
      }
    }
    if (serializedNode.type === NodeType.kAddColumns) {
      const addColumnsNode = node as AddColumnsNode;
      const serializedState = serializedNode.state as {
        secondaryInputNodeId?: string;
      };
      if (serializedState.secondaryInputNodeId) {
        const secondaryInputNode = nodes.get(
          serializedState.secondaryInputNodeId,
        );
        if (secondaryInputNode) {
          addColumnsNode.secondaryInputs.connections.set(0, secondaryInputNode);
        }
      }
    }
    if (serializedNode.type === NodeType.kFilterDuring) {
      const filterDuringNode = node as FilterDuringNode;
      const serializedState = serializedNode.state as {
        secondaryInputNodeIds?: string[];
      };
      const deserializedConnections = FilterDuringNode.deserializeConnections(
        nodes,
        serializedState,
      );
      filterDuringNode.secondaryInputs.connections.clear();
      for (
        let i = 0;
        i < deserializedConnections.secondaryInputNodes.length;
        i++
      ) {
        filterDuringNode.secondaryInputs.connections.set(
          i,
          deserializedConnections.secondaryInputNodes[i],
        );
      }
    }
  }

  // Third pass: resolve columns
  for (const node of nodes.values()) {
    if (node.type === NodeType.kAggregation) {
      (node as AggregationNode).resolveColumns();
    }
    if (node.type === NodeType.kModifyColumns) {
      (node as ModifyColumnsNode).resolveColumns();
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

  // Use provided nodeLayouts if present, otherwise use empty map (will trigger auto-layout)
  const nodeLayouts =
    serializedGraph.nodeLayouts != null
      ? new Map(Object.entries(serializedGraph.nodeLayouts))
      : new Map<string, {x: number; y: number}>();

  return {
    rootNodes,
    selectedNode,
    nodeLayouts,
    labels: serializedGraph.labels,
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
