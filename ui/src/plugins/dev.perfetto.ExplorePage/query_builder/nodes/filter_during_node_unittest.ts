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

import {FilterDuringNode, FilterDuringNodeState} from './filter_during_node';
import {QueryNode, NodeType} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';

describe('FilterDuringNode', () => {
  function createMockNode(id: string, columns: ColumnInfo[]): QueryNode {
    return {
      nodeId: id,
      type: NodeType.kTable,
      nextNodes: [],
      finalCols: columns,
      state: {},
      validate: () => true,
      getTitle: () => `Mock ${id}`,
      nodeSpecificModify: () => ({sections: []}),
      nodeDetails: () => ({content: null}),
      nodeInfo: () => null,
      clone: () => createMockNode(id, columns),
      getStructuredQuery: () => {
        const sq = new protos.PerfettoSqlStructuredQuery();
        sq.id = id;
        sq.table = new protos.PerfettoSqlStructuredQuery.Table();
        sq.table.tableName = 'mock_table';
        sq.table.columnNames = columns.map((c) => c.name);
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
    it('should initialize with default filter settings', () => {
      const node = new FilterDuringNode({});

      expect(node.state.filterNegativeDurPrimary).toBe(true);
      expect(node.state.filterNegativeDurSecondary).toBe(true);
    });

    it('should preserve provided filter settings', () => {
      const node = new FilterDuringNode({
        filterNegativeDurPrimary: false,
        filterNegativeDurSecondary: false,
      });

      expect(node.state.filterNegativeDurPrimary).toBe(false);
      expect(node.state.filterNegativeDurSecondary).toBe(false);
    });

    it('should have correct node type', () => {
      const node = new FilterDuringNode({});

      expect(node.type).toBe(NodeType.kFilterDuring);
    });

    it('should initialize secondary inputs with min=1, max=-1 (unlimited)', () => {
      const node = new FilterDuringNode({});

      expect(node.secondaryInputs.min).toBe(1);
      expect(node.secondaryInputs.max).toBe(-1);
      expect(node.secondaryInputs.connections.size).toBe(0);
    });
  });

  describe('finalCols', () => {
    it('should return empty array when no primary input', () => {
      const node = new FilterDuringNode({});

      expect(node.finalCols).toEqual([]);
    });

    it('should return same columns as primary input', () => {
      const primaryCols = [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('cpu', 'INT'),
      ];
      const primaryNode = createMockNode('primary', primaryCols);

      const node = new FilterDuringNode({});
      node.primaryInput = primaryNode;

      expect(node.finalCols).toEqual(primaryCols);
    });

    it('should preserve column order from primary input', () => {
      const primaryCols = [
        createColumnInfo('cpu', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('id', 'INT'),
        createColumnInfo('dur', 'DURATION'),
        createColumnInfo('name', 'STRING'),
      ];
      const primaryNode = createMockNode('primary', primaryCols);

      const node = new FilterDuringNode({});
      node.primaryInput = primaryNode;

      const finalCols = node.finalCols;
      expect(finalCols.map((c) => c.name)).toEqual([
        'cpu',
        'ts',
        'id',
        'dur',
        'name',
      ]);
    });
  });

  describe('validate', () => {
    it('should fail validation when no primary input', () => {
      const node = new FilterDuringNode({});

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        'No primary input',
      );
    });

    it('should fail validation when no secondary input', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const node = new FilterDuringNode({});
      node.primaryInput = primaryNode;

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        'No interval source',
      );
    });

    it('should fail validation when primary input is invalid', () => {
      const primaryNode = createMockNode('primary', []);
      primaryNode.validate = () => false;

      const secondaryNode = createMockNode('secondary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const node = new FilterDuringNode({});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, secondaryNode);

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        'Primary input is invalid',
      );
    });

    it('should fail validation when secondary input is invalid', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const secondaryNode = createMockNode('secondary', []);
      secondaryNode.validate = () => false;

      const node = new FilterDuringNode({});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, secondaryNode);

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        'Interval source is invalid',
      );
    });

    it('should fail validation when primary input missing required columns', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('name', 'STRING'),
      ]);

      const secondaryNode = createMockNode('secondary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const node = new FilterDuringNode({});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, secondaryNode);

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        'Primary input is missing required columns',
      );
    });

    it('should fail validation when secondary input missing required columns', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const secondaryNode = createMockNode('secondary', [
        createColumnInfo('name', 'STRING'),
      ]);

      const node = new FilterDuringNode({});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, secondaryNode);

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        'Interval source is missing required columns',
      );
    });

    it('should pass validation when all requirements met', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
        createColumnInfo('name', 'STRING'),
      ]);

      const secondaryNode = createMockNode('secondary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const node = new FilterDuringNode({});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, secondaryNode);

      expect(node.validate()).toBe(true);
    });
  });

  describe('getStructuredQuery', () => {
    it('should return undefined when validation fails', () => {
      const node = new FilterDuringNode({});

      expect(node.getStructuredQuery()).toBeUndefined();
    });

    it('should return structured query when valid', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
        createColumnInfo('name', 'STRING'),
      ]);

      const secondaryNode = createMockNode('secondary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const node = new FilterDuringNode({});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, secondaryNode);

      const sq = node.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.id).toBe(node.nodeId);
    });

    it('should create query with correct column selection', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('cpu', 'INT'),
      ]);

      const secondaryNode = createMockNode('secondary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const node = new FilterDuringNode({});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, secondaryNode);

      const sq = node.getStructuredQuery();

      expect(sq?.selectColumns).toBeDefined();
      expect(sq?.selectColumns?.length).toBe(5);

      // Check that columns are in the same order as primary input
      const colNames = sq?.selectColumns?.map(
        (c) => c.alias || c.columnNameOrExpression,
      );
      expect(colNames).toEqual(['id', 'ts', 'dur', 'name', 'cpu']);
    });

    it('should map id from id_0', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const secondaryNode = createMockNode('secondary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const node = new FilterDuringNode({});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, secondaryNode);

      const sq = node.getStructuredQuery();

      // Find the id column in selectColumns
      const idColumn = sq?.selectColumns?.find((c) => c.alias === 'id');
      expect(idColumn?.columnNameOrExpression).toBe('id_0');
      expect(idColumn?.alias).toBe('id');
    });

    it('should use intersected ts and dur without alias', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const secondaryNode = createMockNode('secondary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const node = new FilterDuringNode({});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, secondaryNode);

      const sq = node.getStructuredQuery();

      const tsColumn = sq?.selectColumns?.find(
        (c) => c.columnNameOrExpression === 'ts' && !c.alias,
      );
      const durColumn = sq?.selectColumns?.find(
        (c) => c.columnNameOrExpression === 'dur' && !c.alias,
      );

      expect(tsColumn).toBeDefined();
      expect(durColumn).toBeDefined();
    });

    it('should respect filterNegativeDur settings', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const secondaryNode = createMockNode('secondary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const node = new FilterDuringNode({
        filterNegativeDurPrimary: false,
        filterNegativeDurSecondary: true,
      });
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, secondaryNode);

      const sq = node.getStructuredQuery();

      // The query should be created successfully
      expect(sq).toBeDefined();
    });
  });

  describe('serializeState', () => {
    it('should serialize state correctly', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const secondaryNode = createMockNode('secondary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const node = new FilterDuringNode({
        filterNegativeDurPrimary: false,
        filterNegativeDurSecondary: true,
      });
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, secondaryNode);

      const serialized = node.serializeState();

      expect(serialized).toEqual({
        primaryInputId: primaryNode.nodeId,
        secondaryInputNodeIds: [secondaryNode.nodeId],
        filterNegativeDurPrimary: false,
        filterNegativeDurSecondary: true,
      });
    });

    it('should handle missing inputs gracefully', () => {
      const node = new FilterDuringNode({});

      const serialized = node.serializeState();

      expect(serialized).toEqual({
        primaryInputId: undefined,
        secondaryInputNodeIds: [],
        filterNegativeDurPrimary: true,
        filterNegativeDurSecondary: true,
      });
    });
  });

  describe('clone', () => {
    it('should create a new node with same state', () => {
      const node = new FilterDuringNode({
        filterNegativeDurPrimary: false,
        filterNegativeDurSecondary: true,
      });

      const cloned = node.clone() as FilterDuringNode;

      expect(cloned).toBeInstanceOf(FilterDuringNode);
      expect(
        (cloned.state as FilterDuringNodeState).filterNegativeDurPrimary,
      ).toBe(false);
      expect(
        (cloned.state as FilterDuringNodeState).filterNegativeDurSecondary,
      ).toBe(true);
      expect(cloned.nodeId).not.toBe(node.nodeId); // Should have different ID
    });
  });

  describe('getTitle', () => {
    it('should return correct title', () => {
      const node = new FilterDuringNode({});

      expect(node.getTitle()).toBe('Filter During');
    });
  });

  describe('secondaryNodes getter', () => {
    it('should return empty array when no secondary inputs', () => {
      const node = new FilterDuringNode({});

      expect(node.secondaryNodes).toEqual([]);
    });

    it('should return all connected secondary nodes', () => {
      const secondaryNode1 = createMockNode('secondary1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);
      const secondaryNode2 = createMockNode('secondary2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const node = new FilterDuringNode({});
      node.secondaryInputs.connections.set(0, secondaryNode1);
      node.secondaryInputs.connections.set(1, secondaryNode2);

      expect(node.secondaryNodes).toEqual([secondaryNode1, secondaryNode2]);
    });
  });

  describe('onPrevNodesUpdated', () => {
    it('should trigger onchange callback when called', () => {
      const onchange = jest.fn();
      const node = new FilterDuringNode({
        onchange,
      });

      node.onPrevNodesUpdated();

      expect(onchange).toHaveBeenCalled();
    });

    it('should not throw when onchange is not defined', () => {
      const node = new FilterDuringNode({});

      expect(() => node.onPrevNodesUpdated()).not.toThrow();
    });
  });
});
