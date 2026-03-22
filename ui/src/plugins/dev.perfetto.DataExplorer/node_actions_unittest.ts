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
import {
  NodeActionHandlers,
  createNodeActions,
  createDeferredNodeActions,
  ensureAllNodeActions,
} from './node_actions';
import {createMockNode} from './query_builder/testing/test_utils';

describe('node_actions', () => {
  let handlers: NodeActionHandlers;
  let addAndConnectTableCalls: Array<{
    tableName: string;
    node: QueryNode;
    portIndex: number;
  }>;
  let insertNodeAtPortCalls: Array<{
    node: QueryNode;
    portIndex: number;
    descriptorKey: string;
  }>;

  beforeEach(() => {
    addAndConnectTableCalls = [];
    insertNodeAtPortCalls = [];
    handlers = {
      onAddAndConnectTable: (tableName, node, portIndex) => {
        addAndConnectTableCalls.push({tableName, node, portIndex});
      },
      onInsertNodeAtPort: (node, portIndex, descriptorKey) => {
        insertNodeAtPortCalls.push({node, portIndex, descriptorKey});
      },
    };
  });

  describe('createNodeActions', () => {
    it('should delegate onAddAndConnectTable to handler with node', () => {
      const node = createMockNode({nodeId: 'test-node'});
      const actions = createNodeActions(node, handlers);

      actions.onAddAndConnectTable?.('my_table', 2);

      expect(addAndConnectTableCalls).toHaveLength(1);
      expect(addAndConnectTableCalls[0].tableName).toBe('my_table');
      expect(addAndConnectTableCalls[0].node).toBe(node);
      expect(addAndConnectTableCalls[0].portIndex).toBe(2);
    });

    it('should delegate onInsertModifyColumnsNode with correct descriptor key', () => {
      const node = createMockNode({nodeId: 'test-node'});
      const actions = createNodeActions(node, handlers);

      actions.onInsertModifyColumnsNode?.(1);

      expect(insertNodeAtPortCalls).toHaveLength(1);
      expect(insertNodeAtPortCalls[0].node).toBe(node);
      expect(insertNodeAtPortCalls[0].portIndex).toBe(1);
      expect(insertNodeAtPortCalls[0].descriptorKey).toBe('modify_columns');
    });

    it('should delegate onInsertCounterToIntervalsNode with correct descriptor key', () => {
      const node = createMockNode({nodeId: 'test-node'});
      const actions = createNodeActions(node, handlers);

      actions.onInsertCounterToIntervalsNode?.(3);

      expect(insertNodeAtPortCalls).toHaveLength(1);
      expect(insertNodeAtPortCalls[0].node).toBe(node);
      expect(insertNodeAtPortCalls[0].portIndex).toBe(3);
      expect(insertNodeAtPortCalls[0].descriptorKey).toBe(
        'counter_to_intervals',
      );
    });
  });

  describe('createDeferredNodeActions', () => {
    it('should not call handler when nodeRef is empty', () => {
      const nodeRef: {current?: QueryNode} = {};
      const actions = createDeferredNodeActions(nodeRef, handlers);

      actions.onAddAndConnectTable?.('my_table', 0);
      actions.onInsertModifyColumnsNode?.(0);
      actions.onInsertCounterToIntervalsNode?.(0);

      expect(addAndConnectTableCalls).toHaveLength(0);
      expect(insertNodeAtPortCalls).toHaveLength(0);
    });

    it('should delegate to handler once nodeRef is set', () => {
      const nodeRef: {current?: QueryNode} = {};
      const actions = createDeferredNodeActions(nodeRef, handlers);

      // Set the node reference after creating actions
      const node = createMockNode({nodeId: 'deferred-node'});
      nodeRef.current = node;

      actions.onAddAndConnectTable?.('table_a', 1);

      expect(addAndConnectTableCalls).toHaveLength(1);
      expect(addAndConnectTableCalls[0].node).toBe(node);
      expect(addAndConnectTableCalls[0].tableName).toBe('table_a');
      expect(addAndConnectTableCalls[0].portIndex).toBe(1);
    });

    it('should delegate onInsertModifyColumnsNode once nodeRef is set', () => {
      const nodeRef: {current?: QueryNode} = {};
      const actions = createDeferredNodeActions(nodeRef, handlers);

      const node = createMockNode({nodeId: 'deferred-node'});
      nodeRef.current = node;

      actions.onInsertModifyColumnsNode?.(5);

      expect(insertNodeAtPortCalls).toHaveLength(1);
      expect(insertNodeAtPortCalls[0].node).toBe(node);
      expect(insertNodeAtPortCalls[0].descriptorKey).toBe('modify_columns');
    });

    it('should delegate onInsertCounterToIntervalsNode once nodeRef is set', () => {
      const nodeRef: {current?: QueryNode} = {};
      const actions = createDeferredNodeActions(nodeRef, handlers);

      const node = createMockNode({nodeId: 'deferred-node'});
      nodeRef.current = node;

      actions.onInsertCounterToIntervalsNode?.(7);

      expect(insertNodeAtPortCalls).toHaveLength(1);
      expect(insertNodeAtPortCalls[0].node).toBe(node);
      expect(insertNodeAtPortCalls[0].descriptorKey).toBe(
        'counter_to_intervals',
      );
    });
  });

  describe('ensureAllNodeActions', () => {
    it('should assign actions to nodes that have none', () => {
      const node1 = createMockNode({nodeId: 'n1'});
      const node2 = createMockNode({nodeId: 'n2'});
      const initializedNodes = new Set<string>();

      ensureAllNodeActions([node1, node2], initializedNodes, handlers);

      expect(node1.state.actions).toBeDefined();
      expect(node2.state.actions).toBeDefined();
      expect(initializedNodes.has('n1')).toBe(true);
      expect(initializedNodes.has('n2')).toBe(true);
    });

    it('should skip nodes already in initializedNodes set', () => {
      const node = createMockNode({nodeId: 'n1'});
      const initializedNodes = new Set<string>(['n1']);

      ensureAllNodeActions([node], initializedNodes, handlers);

      // Actions should not be set because the node was already tracked
      expect(node.state.actions).toBeUndefined();
    });

    it('should preserve existing actions on nodes', () => {
      const existingActions = {
        onAddAndConnectTable: () => {},
      };
      const node = createMockNode({
        nodeId: 'n1',
        state: {actions: existingActions},
      });
      const initializedNodes = new Set<string>();

      ensureAllNodeActions([node], initializedNodes, handlers);

      // Existing actions should be preserved (not overwritten)
      expect(node.state.actions).toBe(existingActions);
      // But node should still be marked as initialized
      expect(initializedNodes.has('n1')).toBe(true);
    });

    it('should handle empty node array', () => {
      const initializedNodes = new Set<string>();
      ensureAllNodeActions([], initializedNodes, handlers);
      expect(initializedNodes.size).toBe(0);
    });

    it('should create working actions that delegate to handlers', () => {
      const node = createMockNode({nodeId: 'n1'});
      const initializedNodes = new Set<string>();

      ensureAllNodeActions([node], initializedNodes, handlers);

      // The created actions should actually work
      node.state.actions?.onAddAndConnectTable?.('test_table', 0);

      expect(addAndConnectTableCalls).toHaveLength(1);
      expect(addAndConnectTableCalls[0].tableName).toBe('test_table');
      expect(addAndConnectTableCalls[0].node).toBe(node);
    });
  });
});
