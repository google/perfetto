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
  nextNodeId,
  NodeType,
  singleNodeOperation,
  Query,
  QueryNode,
} from './query_node';
import {queryToRun, isAQuery} from './query_builder/query_builder_utils';
import {notifyNextNodes} from './query_builder/graph_utils';

describe('query_node utilities', () => {
  describe('nextNodeId', () => {
    it('should generate unique node IDs', () => {
      const id1 = nextNodeId();
      const id2 = nextNodeId();
      const id3 = nextNodeId();

      expect(id1).not.toBe(id2);
      expect(id2).not.toBe(id3);
      expect(id1).not.toBe(id3);
    });

    it('should generate numeric string IDs', () => {
      const id = nextNodeId();
      expect(typeof id).toBe('string');
      expect(Number.isNaN(Number(id))).toBe(false);
    });
  });

  describe('singleNodeOperation', () => {
    it('should return true for single node operation types', () => {
      expect(singleNodeOperation(NodeType.kAggregation)).toBe(true);
      expect(singleNodeOperation(NodeType.kModifyColumns)).toBe(true);
      expect(singleNodeOperation(NodeType.kAddColumns)).toBe(true);
      expect(singleNodeOperation(NodeType.kLimitAndOffset)).toBe(true);
      expect(singleNodeOperation(NodeType.kSort)).toBe(true);
    });

    it('should return false for non-single node operation types', () => {
      expect(singleNodeOperation(NodeType.kTable)).toBe(false);
      expect(singleNodeOperation(NodeType.kSimpleSlices)).toBe(false);
      expect(singleNodeOperation(NodeType.kSqlSource)).toBe(false);
      expect(singleNodeOperation(NodeType.kIntervalIntersect)).toBe(false);
      expect(singleNodeOperation(NodeType.kUnion)).toBe(false);
    });
  });

  describe('queryToRun', () => {
    it('should handle undefined query', () => {
      const result = queryToRun(undefined);
      expect(result).toBe('N/A');
    });

    it('should return the SQL string', () => {
      const query: Query = {
        sql: 'SELECT * FROM table',
        textproto: '',
        standaloneSql: '',
      };
      expect(queryToRun(query)).toBe('SELECT * FROM table');
    });
  });

  describe('setOperationChanged', () => {
    function createMockNode(nodeId: string): QueryNode {
      return {
        nodeId,
        type: NodeType.kTable,
        nextNodes: [],
        finalCols: [],
        context: {},
        validate: () => true,
        getTitle: () => 'Test',
        nodeSpecificModify: () => null,
        nodeDetails: () => ({content: null}),
        nodeInfo: () => null,
        clone: () => createMockNode(nodeId),
        getStructuredQuery: () => undefined,
        attrs: {},
      } as QueryNode;
    }

    it('should mark node as changed', () => {
      const node = createMockNode('node1');
      node.context.hasOperationChanged = false;

      node.context.hasOperationChanged = true;

      expect(node.context.hasOperationChanged).toBe(true);
    });

    it('should mark node as changed', () => {
      const node1 = createMockNode('node1');
      const node2 = createMockNode('node2');
      const node3 = createMockNode('node3');
      node1.context.hasOperationChanged = false;
      node2.context.hasOperationChanged = false;
      node3.context.hasOperationChanged = false;

      node1.nextNodes = [node2];
      node2.nextNodes = [node3];

      node1.context.hasOperationChanged = true;

      // Only the node itself should be marked, not children
      // (propagation is handled by QueryExecutionService.invalidateNode)
      expect(node1.context.hasOperationChanged).toBe(true);
      expect(node2.context.hasOperationChanged).toBe(false);
      expect(node3.context.hasOperationChanged).toBe(false);
    });

    it('should mark node as changed even if already changed', () => {
      const node1 = createMockNode('node1');
      node1.context.hasOperationChanged = true;

      node1.context.hasOperationChanged = true;

      expect(node1.context.hasOperationChanged).toBe(true);
    });
  });

  describe('isAQuery', () => {
    it('should return true for valid Query object', () => {
      const query: Query = {
        sql: 'SELECT * FROM table',
        textproto: '',
        standaloneSql: '',
      };

      expect(isAQuery(query)).toBe(true);
    });

    it('should return false for undefined', () => {
      expect(isAQuery(undefined)).toBe(false);
    });

    it('should return false for Error', () => {
      const error = new Error('Something went wrong');
      expect(isAQuery(error)).toBe(false);
    });

    it('should return false for object without sql', () => {
      const notAQuery = {textproto: ''};
      expect(isAQuery(notAQuery as unknown as Query | undefined | Error)).toBe(
        false,
      );
    });
  });

  describe('notifyNextNodes', () => {
    function createPartialNode(
      nodeId: string,
      onPrevNodesUpdated?: () => void,
    ): QueryNode {
      return {
        nodeId,
        type: NodeType.kTable,
        nextNodes: [],
        finalCols: [],
        attrs: {},
        context: {},
        validate: () => true,
        getTitle: () => 'Test',
        nodeSpecificModify: () => null,
        nodeDetails: () => ({content: null}),
        nodeInfo: () => null,
        clone: () => createPartialNode(nodeId, onPrevNodesUpdated),
        getStructuredQuery: () => undefined,
        onPrevNodesUpdated,
      } as QueryNode;
    }

    it('should call onPrevNodesUpdated on next nodes', () => {
      const mockCallback1 = jest.fn();
      const mockCallback2 = jest.fn();

      const node: QueryNode = {
        nodeId: 'node1',
        type: NodeType.kTable,
        nextNodes: [
          createPartialNode('node2', mockCallback1),
          createPartialNode('node3', mockCallback2),
        ],
        finalCols: [],
        attrs: {},
        context: {},
        validate: () => true,
        getTitle: () => 'Test',
        nodeSpecificModify: () => null,
        nodeDetails: () => ({content: null}),
        nodeInfo: () => null,
        clone: () => node,
        getStructuredQuery: () => undefined,
      } as QueryNode;

      notifyNextNodes(node);

      expect(mockCallback1).toHaveBeenCalledTimes(1);
      expect(mockCallback2).toHaveBeenCalledTimes(1);
    });

    it('should handle nodes without onPrevNodesUpdated', () => {
      const node: QueryNode = {
        nodeId: 'node1',
        type: NodeType.kTable,
        nextNodes: [createPartialNode('node2')],
        finalCols: [],
        attrs: {},
        context: {},
        validate: () => true,
        getTitle: () => 'Test',
        nodeSpecificModify: () => null,
        nodeDetails: () => ({content: null}),
        nodeInfo: () => null,
        clone: () => node,
        getStructuredQuery: () => undefined,
      } as QueryNode;

      expect(() => notifyNextNodes(node)).not.toThrow();
    });

    it('should handle empty nextNodes array', () => {
      const node: QueryNode = {
        nodeId: 'node1',
        type: NodeType.kTable,
        nextNodes: [],
        finalCols: [],
        attrs: {},
        context: {},
        validate: () => true,
        getTitle: () => 'Test',
        nodeSpecificModify: () => null,
        nodeDetails: () => ({content: null}),
        nodeInfo: () => null,
        clone: () => node,
        getStructuredQuery: () => undefined,
      } as QueryNode;

      expect(() => notifyNextNodes(node)).not.toThrow();
    });
  });
});
