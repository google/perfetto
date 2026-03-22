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

import {QueryNode, NodeActions} from './query_node';

// Handler functions that NodeActions delegates to.
export interface NodeActionHandlers {
  onAddAndConnectTable: (
    tableName: string,
    node: QueryNode,
    portIndex: number,
  ) => void;
  onInsertNodeAtPort: (
    node: QueryNode,
    portIndex: number,
    descriptorKey: string,
  ) => void;
}

// Creates NodeActions for a node, delegating to the given handlers.
export function createNodeActions(
  node: QueryNode,
  handlers: NodeActionHandlers,
): NodeActions {
  return {
    onAddAndConnectTable: (tableName: string, portIndex: number) => {
      handlers.onAddAndConnectTable(tableName, node, portIndex);
    },
    onInsertModifyColumnsNode: (portIndex: number) => {
      handlers.onInsertNodeAtPort(node, portIndex, 'modify_columns');
    },
    onInsertCounterToIntervalsNode: (portIndex: number) => {
      handlers.onInsertNodeAtPort(node, portIndex, 'counter_to_intervals');
    },
  };
}

// Creates NodeActions using a deferred node reference. Used when the node
// hasn't been created yet (e.g., in handleAddOperationNode).
export function createDeferredNodeActions(
  nodeRef: {current?: QueryNode},
  handlers: NodeActionHandlers,
): NodeActions {
  return {
    onAddAndConnectTable: (tableName: string, portIndex: number) => {
      if (nodeRef.current !== undefined) {
        handlers.onAddAndConnectTable(tableName, nodeRef.current, portIndex);
      }
    },
    onInsertModifyColumnsNode: (portIndex: number) => {
      if (nodeRef.current !== undefined) {
        handlers.onInsertNodeAtPort(
          nodeRef.current,
          portIndex,
          'modify_columns',
        );
      }
    },
    onInsertCounterToIntervalsNode: (portIndex: number) => {
      if (nodeRef.current !== undefined) {
        handlers.onInsertNodeAtPort(
          nodeRef.current,
          portIndex,
          'counter_to_intervals',
        );
      }
    },
  };
}

// Ensures all nodes have their actions initialized. Skips nodes that have
// already been initialized (tracked by initializedNodes set).
export function ensureAllNodeActions(
  nodes: QueryNode[],
  initializedNodes: Set<string>,
  handlers: NodeActionHandlers,
): void {
  for (const node of nodes) {
    if (initializedNodes.has(node.nodeId)) {
      continue;
    }
    if (!node.state.actions) {
      node.state.actions = createNodeActions(node, handlers);
    }
    initializedNodes.add(node.nodeId);
  }
}
