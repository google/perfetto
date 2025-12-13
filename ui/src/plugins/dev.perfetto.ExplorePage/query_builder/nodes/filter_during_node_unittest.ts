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

// Interface for accessing private methods during testing
interface FilterDuringNodeWithPrivates {
  getCommonColumns(): string[];
  cleanupPartitionColumns(): void;
}

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

    it('should initialize with no secondary input', () => {
      const node = new FilterDuringNode({});

      expect(node.secondaryInputs.connections.get(0)).toBeUndefined();
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
        'Connect a node to be filtered to the top port',
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
        'Connect a node with intervals to the port on the left',
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
        'Node to be filtered is invalid',
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
        'Filter intervals input is invalid',
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
        'Node to be filtered is missing required columns',
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
        'Filter intervals input is missing required columns',
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

    it('should pass validation when secondary input has no id column', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
        createColumnInfo('name', 'STRING'),
      ]);

      const secondaryNode = createMockNode('secondary', [
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

    it('should generate query when secondary input has no id column', () => {
      const primaryNode = createMockNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
        createColumnInfo('name', 'STRING'),
      ]);

      const secondaryNode = createMockNode('secondary', [
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
        partitionColumns: undefined,
        clipToIntervals: undefined,
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
        partitionColumns: undefined,
        clipToIntervals: undefined,
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
    it('should return empty array when no secondary input', () => {
      const node = new FilterDuringNode({});

      expect(node.secondaryNodes).toEqual([]);
    });

    it('should return array with single secondary node when connected', () => {
      const secondaryNode = createMockNode('secondary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'TIMESTAMP'),
        createColumnInfo('dur', 'DURATION'),
      ]);

      const node = new FilterDuringNode({});
      node.secondaryInputs.connections.set(0, secondaryNode);

      expect(node.secondaryNodes).toEqual([secondaryNode]);
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

  describe('partition columns', () => {
    describe('initialization', () => {
      it('should initialize with empty partition columns by default', () => {
        const node = new FilterDuringNode({});

        expect(node.state.partitionColumns).toBeUndefined();
      });

      it('should preserve provided partition columns', () => {
        const node = new FilterDuringNode({
          partitionColumns: ['utid', 'cpu'],
        });

        expect(node.state.partitionColumns).toEqual(['utid', 'cpu']);
      });
    });

    describe('getCommonColumns', () => {
      it('should return empty array when no primary input', () => {
        const node = new FilterDuringNode({});

        // Access private method for testing
        const commonColumns = (
          node as unknown as FilterDuringNodeWithPrivates
        ).getCommonColumns();

        expect(commonColumns).toEqual([]);
      });

      it('should return empty array when no secondary inputs', () => {
        const primaryNode = createMockNode('primary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
        ]);

        const node = new FilterDuringNode({});
        node.primaryInput = primaryNode;

        const commonColumns = (
          node as unknown as FilterDuringNodeWithPrivates
        ).getCommonColumns();

        expect(commonColumns).toEqual([]);
      });

      it('should find common columns between primary and secondary inputs', () => {
        const primaryNode = createMockNode('primary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
          createColumnInfo('cpu', 'INT'),
          createColumnInfo('name', 'STRING'),
        ]);

        const secondaryNode = createMockNode('secondary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
          createColumnInfo('cpu', 'INT'),
        ]);

        const node = new FilterDuringNode({});
        node.primaryInput = primaryNode;
        node.secondaryInputs.connections.set(0, secondaryNode);

        const commonColumns = (
          node as unknown as FilterDuringNodeWithPrivates
        ).getCommonColumns();

        expect(commonColumns).toEqual(['cpu', 'utid']);
      });

      it('should exclude id, ts, dur columns', () => {
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

        const commonColumns = (
          node as unknown as FilterDuringNodeWithPrivates
        ).getCommonColumns();

        expect(commonColumns).toEqual([]);
      });

      it('should exclude STRING and BYTES type columns', () => {
        const primaryNode = createMockNode('primary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('name', 'STRING'),
          createColumnInfo('data', 'BYTES'),
          createColumnInfo('utid', 'INT'),
        ]);

        const secondaryNode = createMockNode('secondary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('name', 'STRING'),
          createColumnInfo('data', 'BYTES'),
          createColumnInfo('utid', 'INT'),
        ]);

        const node = new FilterDuringNode({});
        node.primaryInput = primaryNode;
        node.secondaryInputs.connections.set(0, secondaryNode);

        const commonColumns = (
          node as unknown as FilterDuringNodeWithPrivates
        ).getCommonColumns();

        expect(commonColumns).toEqual(['utid']);
      });

      it('should sort common columns alphabetically', () => {
        const primaryNode = createMockNode('primary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('zzz', 'INT'),
          createColumnInfo('aaa', 'INT'),
          createColumnInfo('mmm', 'INT'),
        ]);

        const secondaryNode = createMockNode('secondary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('zzz', 'INT'),
          createColumnInfo('aaa', 'INT'),
          createColumnInfo('mmm', 'INT'),
        ]);

        const node = new FilterDuringNode({});
        node.primaryInput = primaryNode;
        node.secondaryInputs.connections.set(0, secondaryNode);

        const commonColumns = (
          node as unknown as FilterDuringNodeWithPrivates
        ).getCommonColumns();

        expect(commonColumns).toEqual(['aaa', 'mmm', 'zzz']);
      });
    });

    describe('cleanupPartitionColumns', () => {
      it('should not throw when partitionColumns is undefined', () => {
        const node = new FilterDuringNode({});

        expect(() =>
          (
            node as unknown as FilterDuringNodeWithPrivates
          ).cleanupPartitionColumns(),
        ).not.toThrow();
      });

      it('should not throw when partitionColumns is empty', () => {
        const node = new FilterDuringNode({
          partitionColumns: [],
        });

        expect(() =>
          (
            node as unknown as FilterDuringNodeWithPrivates
          ).cleanupPartitionColumns(),
        ).not.toThrow();
      });

      it('should remove partition columns no longer available in inputs', () => {
        const primaryNode = createMockNode('primary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
        ]);

        const secondaryNode = createMockNode('secondary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
        ]);

        const node = new FilterDuringNode({
          partitionColumns: ['utid', 'cpu'], // 'cpu' doesn't exist
        });
        node.primaryInput = primaryNode;
        node.secondaryInputs.connections.set(0, secondaryNode);

        (
          node as unknown as FilterDuringNodeWithPrivates
        ).cleanupPartitionColumns();

        expect(node.state.partitionColumns).toEqual(['utid']);
      });

      it('should clear all partition columns when none are available', () => {
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
          partitionColumns: ['utid', 'cpu'],
        });
        node.primaryInput = primaryNode;
        node.secondaryInputs.connections.set(0, secondaryNode);

        (
          node as unknown as FilterDuringNodeWithPrivates
        ).cleanupPartitionColumns();

        expect(node.state.partitionColumns).toEqual([]);
      });

      it('should preserve valid partition columns', () => {
        const primaryNode = createMockNode('primary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
          createColumnInfo('cpu', 'INT'),
        ]);

        const secondaryNode = createMockNode('secondary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
          createColumnInfo('cpu', 'INT'),
        ]);

        const node = new FilterDuringNode({
          partitionColumns: ['utid', 'cpu'],
        });
        node.primaryInput = primaryNode;
        node.secondaryInputs.connections.set(0, secondaryNode);

        (
          node as unknown as FilterDuringNodeWithPrivates
        ).cleanupPartitionColumns();

        expect(node.state.partitionColumns).toEqual(['utid', 'cpu']);
      });
    });

    describe('serializeState with partition columns', () => {
      it('should include partition columns in serialized state', () => {
        const primaryNode = createMockNode('primary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
        ]);

        const secondaryNode = createMockNode('secondary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
        ]);

        const node = new FilterDuringNode({
          partitionColumns: ['utid'],
        });
        node.primaryInput = primaryNode;
        node.secondaryInputs.connections.set(0, secondaryNode);

        const serialized = node.serializeState();

        expect(serialized).toEqual({
          primaryInputId: primaryNode.nodeId,
          secondaryInputNodeIds: [secondaryNode.nodeId],
          filterNegativeDurPrimary: true,
          filterNegativeDurSecondary: true,
          partitionColumns: ['utid'],
          clipToIntervals: undefined,
        });
      });

      it('should handle undefined partition columns', () => {
        const node = new FilterDuringNode({});

        const serialized = node.serializeState() as Record<string, unknown>;

        expect(serialized).toHaveProperty('partitionColumns');
        expect(serialized.partitionColumns).toBeUndefined();
      });
    });

    describe('deserializeState with partition columns', () => {
      it('should restore partition columns from serialized state', () => {
        const state = FilterDuringNode.deserializeState({
          filterNegativeDurPrimary: false,
          filterNegativeDurSecondary: true,
          partitionColumns: ['utid', 'cpu'],
        });

        expect(state.partitionColumns).toEqual(['utid', 'cpu']);
      });

      it('should handle missing partition columns in serialized state', () => {
        const state = FilterDuringNode.deserializeState({
          filterNegativeDurPrimary: false,
          filterNegativeDurSecondary: true,
        });

        expect(state.partitionColumns).toBeUndefined();
      });
    });

    describe('clone with partition columns', () => {
      it('should clone partition columns', () => {
        const node = new FilterDuringNode({
          partitionColumns: ['utid', 'cpu'],
        });

        const cloned = node.clone() as FilterDuringNode;

        expect(cloned.state.partitionColumns).toEqual(['utid', 'cpu']);
      });

      it('should create independent copy of partition columns array', () => {
        const node = new FilterDuringNode({
          partitionColumns: ['utid'],
        });

        const cloned = node.clone() as FilterDuringNode;

        // Modify cloned partition columns
        cloned.state.partitionColumns?.push('cpu');

        // Original should not be affected
        expect(node.state.partitionColumns).toEqual(['utid']);
        expect(cloned.state.partitionColumns).toEqual(['utid', 'cpu']);
      });

      it('should handle undefined partition columns', () => {
        const node = new FilterDuringNode({});

        const cloned = node.clone() as FilterDuringNode;

        expect(cloned.state.partitionColumns).toBeUndefined();
      });
    });

    describe('getStructuredQuery with partition columns', () => {
      it('should pass partition columns to interval intersect', () => {
        const primaryNode = createMockNode('primary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
        ]);

        const secondaryNode = createMockNode('secondary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
        ]);

        const node = new FilterDuringNode({
          partitionColumns: ['utid'],
        });
        node.primaryInput = primaryNode;
        node.secondaryInputs.connections.set(0, secondaryNode);

        const sq = node.getStructuredQuery();

        // Query should be generated successfully with partition columns
        expect(sq).toBeDefined();
        expect(sq?.id).toBe(node.nodeId);
      });

      it('should work without partition columns', () => {
        const primaryNode = createMockNode('primary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
        ]);

        const secondaryNode = createMockNode('secondary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
        ]);

        const node = new FilterDuringNode({});
        node.primaryInput = primaryNode;
        node.secondaryInputs.connections.set(0, secondaryNode);

        const sq = node.getStructuredQuery();

        expect(sq).toBeDefined();
        expect(sq?.id).toBe(node.nodeId);
      });
    });

    describe('onPrevNodesUpdated with partition columns', () => {
      it('should cleanup partition columns when inputs change', () => {
        const primaryNode = createMockNode('primary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
        ]);

        const secondaryNode = createMockNode('secondary', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('ts', 'TIMESTAMP'),
          createColumnInfo('dur', 'DURATION'),
          createColumnInfo('utid', 'INT'),
        ]);

        const node = new FilterDuringNode({
          partitionColumns: ['utid', 'cpu'], // 'cpu' doesn't exist
        });
        node.primaryInput = primaryNode;
        node.secondaryInputs.connections.set(0, secondaryNode);

        node.onPrevNodesUpdated();

        // 'cpu' should be removed as it doesn't exist in inputs
        expect(node.state.partitionColumns).toEqual(['utid']);
      });
    });
  });
});
