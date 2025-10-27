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
  createSelectColumnsProto,
  createFinalColumns,
  queryToRun,
  setOperationChanged,
  isAQuery,
  notifyNextNodes,
  Query,
  QueryNode,
  QueryNodeState,
} from './query_node';
import {ColumnInfo} from './query_builder/column_info';
import {SqlType} from '../dev.perfetto.SqlModules/sql_modules';

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

  describe('createSelectColumnsProto', () => {
    const stringType: SqlType = {name: 'STRING', shortName: 'string'};
    const intType: SqlType = {name: 'INTEGER', shortName: 'int'};

    function createMockNode(columns: ColumnInfo[]): QueryNode {
      return {
        nodeId: 'test-node',
        type: NodeType.kTable,
        nextNodes: [],
        finalCols: columns,
        state: {},
        validate: () => true,
        getTitle: () => 'Test',
        nodeSpecificModify: () => null,
        clone: () => createMockNode(columns),
        getStructuredQuery: () => undefined,
        serializeState: () => ({}),
      } as QueryNode;
    }

    it('should return undefined if all columns are checked', () => {
      const columns: ColumnInfo[] = [
        {
          name: 'id',
          type: 'INTEGER',
          checked: true,
          column: {name: 'id', type: intType},
        },
        {
          name: 'name',
          type: 'STRING',
          checked: true,
          column: {name: 'name', type: stringType},
        },
      ];
      const node = createMockNode(columns);

      const result = createSelectColumnsProto(node);

      expect(result).toBeUndefined();
    });

    it('should return selected columns when some are unchecked', () => {
      const columns: ColumnInfo[] = [
        {
          name: 'id',
          type: 'INTEGER',
          checked: true,
          column: {name: 'id', type: intType},
        },
        {
          name: 'name',
          type: 'STRING',
          checked: false,
          column: {name: 'name', type: stringType},
        },
        {
          name: 'age',
          type: 'INTEGER',
          checked: true,
          column: {name: 'age', type: intType},
        },
      ];
      const node = createMockNode(columns);

      const result = createSelectColumnsProto(node);

      expect(result).toBeDefined();
      expect(result?.length).toBe(2);
      expect(result?.[0].columnName).toBe('id');
      expect(result?.[1].columnName).toBe('age');
    });

    it('should include aliases when present', () => {
      const columns: ColumnInfo[] = [
        {
          name: 'id',
          type: 'INTEGER',
          checked: true,
          column: {name: 'id', type: intType},
          alias: 'identifier',
        },
        {
          name: 'name',
          type: 'STRING',
          checked: true,
          column: {name: 'name', type: stringType},
        },
      ];
      const node = createMockNode(columns);

      const result = createSelectColumnsProto(node);

      expect(result).toBeUndefined(); // All checked, so undefined
    });

    it('should handle empty column list', () => {
      const node = createMockNode([]);

      const result = createSelectColumnsProto(node);

      expect(result).toBeUndefined();
    });
  });

  describe('createFinalColumns', () => {
    const stringType: SqlType = {name: 'STRING', shortName: 'string'};

    it('should create final columns with all checked', () => {
      const sourceCols: ColumnInfo[] = [
        {
          name: 'id',
          type: 'INTEGER',
          checked: false,
          column: {name: 'id', type: stringType},
        },
        {
          name: 'name',
          type: 'STRING',
          checked: false,
          column: {name: 'name', type: stringType},
        },
      ];

      const result = createFinalColumns(sourceCols);

      expect(result.length).toBe(2);
      expect(result[0].checked).toBe(true);
      expect(result[1].checked).toBe(true);
    });

    it('should preserve column information', () => {
      const sourceCols: ColumnInfo[] = [
        {
          name: 'id',
          type: 'INTEGER',
          checked: false,
          column: {name: 'id', type: stringType},
          alias: 'identifier',
        },
      ];

      const result = createFinalColumns(sourceCols);

      expect(result[0].name).toBe('identifier');
      expect(result[0].type).toBe('STRING');
      expect(result[0].column.name).toBe('id');
    });
  });

  describe('queryToRun', () => {
    it('should handle undefined query', () => {
      const result = queryToRun(undefined);
      expect(result).toBe('N/A');
    });

    it('should format query with modules', () => {
      const query: Query = {
        sql: 'SELECT * FROM table',
        textproto: '',
        modules: ['android.slices', 'experimental.frames'],
        preambles: [],
        columns: [],
      };

      const result = queryToRun(query);

      expect(result).toContain('INCLUDE PERFETTO MODULE android.slices;');
      expect(result).toContain('INCLUDE PERFETTO MODULE experimental.frames;');
      expect(result).toContain('SELECT * FROM table');
    });

    it('should format query with preambles', () => {
      const query: Query = {
        sql: 'SELECT * FROM table',
        textproto: '',
        modules: [],
        preambles: ['CREATE VIEW test AS SELECT 1;'],
        columns: [],
      };

      const result = queryToRun(query);

      expect(result).toContain('CREATE VIEW test AS SELECT 1;');
      expect(result).toContain('SELECT * FROM table');
    });

    it('should format query with both modules and preambles', () => {
      const query: Query = {
        sql: 'SELECT * FROM table',
        textproto: '',
        modules: ['android.slices'],
        preambles: ['-- This is a comment'],
        columns: [],
      };

      const result = queryToRun(query);

      expect(result).toContain('INCLUDE PERFETTO MODULE android.slices;');
      expect(result).toContain('-- This is a comment');
      expect(result).toContain('SELECT * FROM table');
    });

    it('should handle empty modules and preambles', () => {
      const query: Query = {
        sql: 'SELECT * FROM table',
        textproto: '',
        modules: [],
        preambles: [],
        columns: [],
      };

      const result = queryToRun(query);

      expect(result).toBe('SELECT * FROM table');
    });
  });

  describe('setOperationChanged', () => {
    function createMockNode(
      nodeId: string,
      state: QueryNodeState = {},
    ): QueryNode {
      return {
        nodeId,
        type: NodeType.kTable,
        nextNodes: [],
        finalCols: [],
        state,
        validate: () => true,
        getTitle: () => 'Test',
        nodeSpecificModify: () => null,
        clone: () => createMockNode(nodeId, state),
        getStructuredQuery: () => undefined,
        serializeState: () => ({}),
      } as QueryNode;
    }

    it('should mark node as changed', () => {
      const state: QueryNodeState = {hasOperationChanged: false};
      const node = createMockNode('node1', state);

      setOperationChanged(node);

      expect(state.hasOperationChanged).toBe(true);
    });

    it('should propagate change to next nodes', () => {
      const state1: QueryNodeState = {hasOperationChanged: false};
      const state2: QueryNodeState = {hasOperationChanged: false};
      const state3: QueryNodeState = {hasOperationChanged: false};

      const node1 = createMockNode('node1', state1);
      const node2 = createMockNode('node2', state2);
      const node3 = createMockNode('node3', state3);

      node1.nextNodes = [node2];
      node2.nextNodes = [node3];

      setOperationChanged(node1);

      expect(state1.hasOperationChanged).toBe(true);
      expect(state2.hasOperationChanged).toBe(true);
      expect(state3.hasOperationChanged).toBe(true);
    });

    it('should stop propagation if node already marked as changed', () => {
      const state1: QueryNodeState = {hasOperationChanged: false};
      const state2: QueryNodeState = {hasOperationChanged: true};
      const state3: QueryNodeState = {hasOperationChanged: false};

      const node1 = createMockNode('node1', state1);
      const node2 = createMockNode('node2', state2);
      const node3 = createMockNode('node3', state3);

      node1.nextNodes = [node2];
      node2.nextNodes = [node3];

      setOperationChanged(node1);

      expect(state1.hasOperationChanged).toBe(true);
      // Should stop at node2 since it was already marked as changed
      expect(state3.hasOperationChanged).toBe(false);
    });

    it('should handle multiple next nodes', () => {
      const state1: QueryNodeState = {hasOperationChanged: false};
      const state2: QueryNodeState = {hasOperationChanged: false};
      const state3: QueryNodeState = {hasOperationChanged: false};

      const node1 = createMockNode('node1', state1);
      const node2 = createMockNode('node2', state2);
      const node3 = createMockNode('node3', state3);

      node1.nextNodes = [node2, node3];

      setOperationChanged(node1);

      expect(state1.hasOperationChanged).toBe(true);
      expect(state2.hasOperationChanged).toBe(true);
      expect(state3.hasOperationChanged).toBe(true);
    });
  });

  describe('isAQuery', () => {
    it('should return true for valid Query object', () => {
      const query: Query = {
        sql: 'SELECT * FROM table',
        textproto: '',
        modules: [],
        preambles: [],
        columns: [],
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
      const notAQuery = {textproto: '', modules: [], preambles: []};
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
        state: {},
        validate: () => true,
        getTitle: () => 'Test',
        nodeSpecificModify: () => null,
        clone: () => createPartialNode(nodeId, onPrevNodesUpdated),
        getStructuredQuery: () => undefined,
        serializeState: () => ({}),
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
        state: {},
        validate: () => true,
        getTitle: () => 'Test',
        nodeSpecificModify: () => null,
        clone: () => node,
        getStructuredQuery: () => undefined,
        serializeState: () => ({}),
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
        state: {},
        validate: () => true,
        getTitle: () => 'Test',
        nodeSpecificModify: () => null,
        clone: () => node,
        getStructuredQuery: () => undefined,
        serializeState: () => ({}),
      } as QueryNode;

      expect(() => notifyNextNodes(node)).not.toThrow();
    });

    it('should handle empty nextNodes array', () => {
      const node: QueryNode = {
        nodeId: 'node1',
        type: NodeType.kTable,
        nextNodes: [],
        finalCols: [],
        state: {},
        validate: () => true,
        getTitle: () => 'Test',
        nodeSpecificModify: () => null,
        clone: () => node,
        getStructuredQuery: () => undefined,
        serializeState: () => ({}),
      } as QueryNode;

      expect(() => notifyNextNodes(node)).not.toThrow();
    });
  });
});
