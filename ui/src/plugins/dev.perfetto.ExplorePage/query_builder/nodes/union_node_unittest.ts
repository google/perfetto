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

import {UnionNode} from './union_node';
import {QueryNode, NodeType} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';

describe('UnionNode', () => {
  function createMockNode(id: string, columns: ColumnInfo[]): QueryNode {
    return {
      nodeId: id,
      type: NodeType.kTable,
      nextNodes: [],
      finalCols: columns,
      state: {},
      validate: () => true,
      getTitle: () => `Mock ${id}`,
      nodeSpecificModify: () => null,
      nodeDetails: () => ({content: null}),
      nodeInfo: () => null,
      clone: () => createMockNode(id, columns),
      getStructuredQuery: () => {
        const sq = new protos.PerfettoSqlStructuredQuery();
        sq.id = id;
        return sq;
      },
      serializeState: () => ({}),
    } as QueryNode;
  }

  function createColumnInfo(
    name: string,
    type: string,
    checked: boolean = true,
  ): ColumnInfo {
    return {
      name,
      type,
      checked,
      column: {name},
    };
  }

  describe('constructor', () => {
    it('should initialize with default values', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [
          createColumnInfo('id', 'INT'),
          createColumnInfo('name', 'STRING'),
        ],
      });

      expect(unionNode.state.autoExecute).toBe(false);
      expect(unionNode.state.selectedColumns.length).toBe(2);
      expect(unionNode.secondaryInputs.min).toBe(2);
      expect(unionNode.secondaryInputs.max).toBe('unbounded');
    });

    it('should initialize connections from inputNodes', () => {
      const node1 = createMockNode('node1', []);
      const node2 = createMockNode('node2', []);
      const node3 = createMockNode('node3', []);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2, node3],
        selectedColumns: [],
      });

      expect(unionNode.secondaryInputs.connections.size).toBe(3);
      expect(unionNode.secondaryInputs.connections.get(0)).toBe(node1);
      expect(unionNode.secondaryInputs.connections.get(1)).toBe(node2);
      expect(unionNode.secondaryInputs.connections.get(2)).toBe(node3);
    });
  });

  describe('getCommonColumns', () => {
    it('should return empty array when there are no input nodes', () => {
      const unionNode = new UnionNode({
        inputNodes: [],
        selectedColumns: [],
      });

      expect(unionNode['getCommonColumns']()).toEqual([]);
    });

    it('should return all columns from single input', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1],
        selectedColumns: [],
      });

      const commonCols = unionNode['getCommonColumns']();
      expect(commonCols.length).toBe(2);
      expect(commonCols.map((c) => c.column.name)).toEqual(['id', 'name']);
    });

    it('should return only common columns between two inputs', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('value', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [],
      });

      const commonCols = unionNode['getCommonColumns']();
      expect(commonCols.length).toBe(2);
      expect(commonCols.map((c) => c.column.name)).toEqual(['id', 'name']);
    });

    it('should return only columns present in all inputs', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('value', 'INT'),
      ]);
      const node3 = createMockNode('node3', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('other', 'STRING'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2, node3],
        selectedColumns: [],
      });

      const commonCols = unionNode['getCommonColumns']();
      expect(commonCols.length).toBe(1);
      expect(commonCols.map((c) => c.column.name)).toEqual(['id']);
    });

    it('should return empty array when there are no common columns', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('value', 'INT'),
        createColumnInfo('ts', 'INT64'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [],
      });

      const commonCols = unionNode['getCommonColumns']();
      expect(commonCols.length).toBe(0);
    });

    it('should set all columns as checked by default', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [],
      });

      const commonCols = unionNode['getCommonColumns']();
      expect(commonCols.every((c) => c.checked === true)).toBe(true);
    });
  });

  describe('finalCols', () => {
    it('should return only checked columns', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [
          createColumnInfo('id', 'INT', true),
          createColumnInfo('name', 'STRING', false),
        ],
      });

      const finalCols = unionNode.finalCols;
      expect(finalCols.length).toBe(1);
      expect(finalCols[0].column.name).toBe('id');
    });

    it('should return empty array when no columns are checked', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [createColumnInfo('id', 'INT', false)],
      });

      expect(unionNode.finalCols).toEqual([]);
    });
  });

  describe('onPrevNodesUpdated', () => {
    it('should update selectedColumns based on new common columns', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [
          createColumnInfo('id', 'INT', true),
          createColumnInfo('name', 'STRING', false),
        ],
      });

      // Update node2 to remove 'name'
      const updatedNode2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('value', 'INT'),
      ]);
      unionNode.secondaryInputs.connections.set(1, updatedNode2);

      unionNode.onPrevNodesUpdated();

      // Only 'id' should remain as it's the only common column
      expect(unionNode.state.selectedColumns.length).toBe(1);
      expect(unionNode.state.selectedColumns[0].column.name).toBe('id');
      // 'id' should preserve its checked status (true)
      expect(unionNode.state.selectedColumns[0].checked).toBe(true);
    });

    it('should preserve checked status for columns that still exist', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [
          createColumnInfo('id', 'INT', true),
          createColumnInfo('name', 'STRING', false),
          createColumnInfo('ts', 'INT64', true),
        ],
      });

      unionNode.onPrevNodesUpdated();

      // All columns should still exist
      expect(unionNode.state.selectedColumns.length).toBe(3);

      // Check that checked status is preserved
      const idCol = unionNode.state.selectedColumns.find(
        (c) => c.column.name === 'id',
      );
      const nameCol = unionNode.state.selectedColumns.find(
        (c) => c.column.name === 'name',
      );
      const tsCol = unionNode.state.selectedColumns.find(
        (c) => c.column.name === 'ts',
      );

      expect(idCol?.checked).toBe(true);
      expect(nameCol?.checked).toBe(false);
      expect(tsCol?.checked).toBe(true);
    });
  });

  describe('validate', () => {
    it('should fail when there are fewer than 2 input nodes', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1],
        selectedColumns: [],
      });

      expect(unionNode.validate()).toBe(false);
      expect(unionNode.state.issues?.queryError?.message).toContain(
        'at least two sources',
      );
    });

    it('should fail when there are no common columns', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('value', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [],
      });

      expect(unionNode.validate()).toBe(false);
      expect(unionNode.state.issues?.queryError?.message).toContain(
        'common columns',
      );
    });

    it('should fail when input nodes have disconnected inputs', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1],
        selectedColumns: [],
      });

      // Manually add undefined to connections
      unionNode.secondaryInputs.connections.set(1, undefined as any);

      expect(unionNode.validate()).toBe(false);
      expect(unionNode.state.issues?.queryError?.message).toContain(
        'disconnected inputs',
      );
    });

    it('should pass validation with valid inputs', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [
          createColumnInfo('id', 'INT'),
          createColumnInfo('name', 'STRING'),
        ],
      });

      expect(unionNode.validate()).toBe(true);
    });

    it('should clear previous errors on successful validation', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [createColumnInfo('id', 'INT')],
      });

      // First validate with error
      unionNode.secondaryInputs.connections.clear();
      expect(unionNode.validate()).toBe(false);
      expect(unionNode.state.issues?.queryError).toBeDefined();

      // Restore connections and validate again
      unionNode.secondaryInputs.connections.set(0, node1);
      unionNode.secondaryInputs.connections.set(1, node2);
      expect(unionNode.validate()).toBe(true);
      expect(unionNode.state.issues?.queryError).toBeUndefined();
    });
  });

  describe('getTitle', () => {
    it('should return "Union"', () => {
      const unionNode = new UnionNode({
        inputNodes: [],
        selectedColumns: [],
      });

      expect(unionNode.getTitle()).toBe('Union');
    });
  });

  describe('clone', () => {
    it('should create a deep copy of the node', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [createColumnInfo('id', 'INT', true)],
      });
      unionNode.comment = 'test comment';

      const cloned = unionNode.clone() as UnionNode;

      expect(cloned).not.toBe(unionNode);
      expect(cloned.state.inputNodes).toEqual(unionNode.state.inputNodes);
      expect(cloned.state.selectedColumns.length).toBe(1);
      expect(cloned.comment).toBe('test comment');
    });

    it('should not share state with original', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [createColumnInfo('id', 'INT', true)],
      });

      const cloned = unionNode.clone() as UnionNode;

      // Modify the cloned state
      cloned.state.selectedColumns[0].checked = false;

      // Original should not be affected
      expect(unionNode.state.selectedColumns[0].checked).toBe(true);
    });
  });

  describe('getStructuredQuery', () => {
    it('should return undefined when there are fewer than 2 inputs', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1],
        selectedColumns: [],
      });

      expect(unionNode.getStructuredQuery()).toBeUndefined();
    });

    it('should return undefined when there are no checked columns', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [createColumnInfo('id', 'INT', false)],
      });

      expect(unionNode.getStructuredQuery()).toBeUndefined();
    });

    it('should return undefined when any input node is undefined', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1],
        selectedColumns: [createColumnInfo('id', 'INT')],
      });

      unionNode.secondaryInputs.connections.set(1, undefined as any);

      expect(unionNode.getStructuredQuery()).toBeUndefined();
    });

    it('should create union query with wrapped selects for common columns', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('value', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [
          createColumnInfo('id', 'INT', true),
          createColumnInfo('name', 'STRING', true),
        ],
      });

      const sq = unionNode.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.experimentalUnion).toBeDefined();
      expect(sq?.experimentalUnion?.useUnionAll).toBe(true);
      expect(sq?.experimentalUnion?.queries?.length).toBe(2);
    });

    it('should only select checked common columns', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [
          createColumnInfo('id', 'INT', true),
          createColumnInfo('name', 'STRING', false), // unchecked
          createColumnInfo('ts', 'INT64', true),
        ],
      });

      const sq = unionNode.getStructuredQuery();

      expect(sq).toBeDefined();
      // The union queries should be wrapped with SELECT that only includes id and ts
      const queries = sq?.experimentalUnion?.queries;
      expect(queries?.length).toBe(2);

      // Each wrapped query should have selectColumns for only checked columns
      queries?.forEach((query) => {
        expect(query.selectColumns?.length).toBe(2);
        const colNames = query.selectColumns?.map(
          (c) => c.columnNameOrExpression,
        );
        expect(colNames).toContain('id');
        expect(colNames).toContain('ts');
        expect(colNames).not.toContain('name');
      });
    });
  });

  describe('serializeState', () => {
    it('should serialize all input node IDs and selected columns', () => {
      const node1 = createMockNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockNode('node2', [
        createColumnInfo('id', 'INT'),
      ]);

      const unionNode = new UnionNode({
        inputNodes: [node1, node2],
        selectedColumns: [createColumnInfo('id', 'INT', true)],
      });
      unionNode.comment = 'test comment';

      const serialized = unionNode.serializeState();

      expect(serialized.unionNodes).toEqual(['node1', 'node2']);
      expect(serialized.selectedColumns.length).toBe(1);
      expect(serialized.selectedColumns[0].column.name).toBe('id');
      expect(serialized.comment).toBe('test comment');
    });
  });

  describe('deserializeState', () => {
    it('should deserialize state correctly', () => {
      const serialized = {
        unionNodes: ['node1', 'node2'],
        selectedColumns: [createColumnInfo('id', 'INT', true)],
        comment: 'test comment',
      };

      const state = UnionNode.deserializeState(serialized);

      expect(state.inputNodes).toEqual([]);
      expect(state.selectedColumns.length).toBe(1);
      expect(state.selectedColumns[0].column.name).toBe('id');
    });
  });

  describe('deserializeConnections', () => {
    it('should deserialize connections correctly', () => {
      const node1 = createMockNode('node1', []);
      const node2 = createMockNode('node2', []);
      const node3 = createMockNode('node3', []);
      const nodes = new Map([
        ['node1', node1],
        ['node2', node2],
        ['node3', node3],
      ]);

      const connections = UnionNode.deserializeConnections(nodes, {
        unionNodes: ['node1', 'node2', 'node3'],
        selectedColumns: [],
      });

      expect(connections.inputNodes.length).toBe(3);
      expect(connections.inputNodes[0]).toBe(node1);
      expect(connections.inputNodes[1]).toBe(node2);
      expect(connections.inputNodes[2]).toBe(node3);
    });

    it('should handle missing nodes gracefully', () => {
      const node1 = createMockNode('node1', []);
      const nodes = new Map([['node1', node1]]);

      const connections = UnionNode.deserializeConnections(nodes, {
        unionNodes: ['node1', 'missing1', 'node2'],
        selectedColumns: [],
      });

      // Should only include node1, filtering out undefined entries
      expect(connections.inputNodes.length).toBe(1);
      expect(connections.inputNodes[0]).toBe(node1);
    });
  });
});
