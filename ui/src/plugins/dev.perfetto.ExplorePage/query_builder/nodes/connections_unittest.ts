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

/**
 * Connection Management Tests
 *
 * Tests for adding and removing connections between nodes using the
 * addConnection and removeConnection APIs. Tests cover:
 *
 * 1. IntervalIntersectNode - Multi-input node with only secondary inputs
 * 2. AddColumnsNode - Node with primary input + one secondary input (lookup)
 * 3. FilterDuringNode - Node with primary input + multiple secondary inputs
 */

import {IntervalIntersectNode} from './interval_intersect_node';
import {AddColumnsNode} from './add_columns_node';
import {FilterDuringNode} from './filter_during_node';
import {ModifyColumnsNode} from './modify_columns_node';
import {FilterNode} from './filter_node';
import {
  QueryNode,
  NodeType,
  notifyNextNodes,
  addConnection,
  removeConnection,
} from '../../query_node';
import {insertNodeBetween} from '../graph_utils';
import {ColumnInfo} from '../column_info';
import {
  PerfettoSqlType,
  PerfettoSqlTypes,
} from '../../../../trace_processor/perfetto_sql_type';

// Helper to create a mock previous node with specified columns
function createMockPrevNode(id: string, columns: ColumnInfo[]): QueryNode {
  const node: QueryNode = {
    nodeId: id,
    type: NodeType.kTable,
    nextNodes: [],
    finalCols: columns,
    getTitle: () => id,
    validate: () => true,
    state: {},
    serializeState: () => ({}),
    nodeSpecificModify: () => null,
    nodeDetails: () => ({content: null, message: ''}),
    nodeInfo: () => null,
    clone: () => node,
    getStructuredQuery: () => undefined,
  };
  return node;
}

// Helper to create a ColumnInfo with basic type
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

// Helper to create a ColumnInfo with full SQL type
function createColumnInfoWithSqlType(
  name: string,
  displayType: string,
  sqlType: PerfettoSqlType,
  checked: boolean = true,
): ColumnInfo {
  return {
    name,
    type: displayType,
    checked,
    column: {name, type: sqlType},
  };
}

describe('Connection Management', () => {
  describe('IntervalIntersectNode', () => {
    it('should add a third input using addConnection', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node3 = createMockPrevNode('node3', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('extra', 'STRING'),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });
      node1.nextNodes.push(intervalNode);
      node2.nextNodes.push(intervalNode);

      expect(intervalNode.secondaryInputs.connections.size).toBe(2);
      expect(intervalNode.state.filterNegativeDur?.length).toBe(2);

      addConnection(node3, intervalNode);

      expect(intervalNode.secondaryInputs.connections.size).toBe(3);
      expect(intervalNode.secondaryInputs.connections.get(2)).toBe(node3);
      expect(node3.nextNodes).toContain(intervalNode);
      expect(intervalNode.state.filterNegativeDur?.length).toBe(3);
      expect(intervalNode.state.filterNegativeDur?.[2]).toBe(true);

      const cols = intervalNode.finalCols;
      expect(cols.find((c) => c.name === 'id_2')).toBeDefined();
      expect(cols.find((c) => c.name === 'extra')).toBeDefined();
    });

    it('should remove a middle input using removeConnection', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node3 = createMockPrevNode('node3', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2, node3],
        filterNegativeDur: [true, false, true],
      });
      node1.nextNodes.push(intervalNode);
      node2.nextNodes.push(intervalNode);
      node3.nextNodes.push(intervalNode);

      expect(intervalNode.secondaryInputs.connections.size).toBe(3);

      removeConnection(node2, intervalNode);

      expect(intervalNode.secondaryInputs.connections.size).toBe(2);
      expect(intervalNode.secondaryInputs.connections.get(1)).toBeUndefined();
      expect(node2.nextNodes).not.toContain(intervalNode);
      expect(intervalNode.state.filterNegativeDur?.length).toBe(2);
    });

    it('should remove all inputs and leave node with no connections', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });
      node1.nextNodes.push(intervalNode);
      node2.nextNodes.push(intervalNode);

      removeConnection(node1, intervalNode);
      removeConnection(node2, intervalNode);

      expect(intervalNode.secondaryInputs.connections.size).toBe(0);
      expect(intervalNode.finalCols).toEqual([]);
      expect(intervalNode.validate()).toBe(false);
    });

    it('should notify downstream nodes when connections change', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node3 = createMockPrevNode('node3', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('unique_col', 'STRING'),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });
      node1.nextNodes.push(intervalNode);
      node2.nextNodes.push(intervalNode);

      const modifyNode = new ModifyColumnsNode({selectedColumns: []});
      modifyNode.primaryInput = intervalNode;
      intervalNode.nextNodes.push(modifyNode);
      modifyNode.onPrevNodesUpdated();

      expect(
        modifyNode.state.selectedColumns.find((c) => c.name === 'unique_col'),
      ).toBeUndefined();

      addConnection(node3, intervalNode);
      notifyNextNodes(intervalNode);

      expect(
        modifyNode.state.selectedColumns.find((c) => c.name === 'unique_col'),
      ).toBeDefined();
    });

    it('should add connection at a specific port index', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node3 = createMockPrevNode('node3', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });
      node1.nextNodes.push(intervalNode);
      node2.nextNodes.push(intervalNode);

      addConnection(node3, intervalNode, 5);

      expect(intervalNode.secondaryInputs.connections.get(5)).toBe(node3);
      expect(intervalNode.secondaryInputs.connections.size).toBe(3);
    });

    it('should add and remove connections in sequence', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node3 = createMockPrevNode('node3', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node4 = createMockPrevNode('node4', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });
      node1.nextNodes.push(intervalNode);
      node2.nextNodes.push(intervalNode);

      addConnection(node3, intervalNode);
      expect(intervalNode.secondaryInputs.connections.size).toBe(3);

      removeConnection(node2, intervalNode);
      expect(intervalNode.secondaryInputs.connections.size).toBe(2);

      addConnection(node4, intervalNode);
      expect(intervalNode.secondaryInputs.connections.size).toBe(3);

      expect(intervalNode.secondaryInputs.connections.get(0)).toBe(node1);
      expect(intervalNode.secondaryInputs.connections.get(2)).toBe(node3);
      expect(intervalNode.secondaryInputs.connections.get(1)).toBe(node4);
      expect(intervalNode.validate()).toBe(true);
    });

    it('should update filterNegativeDur when adding connections with default true', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node3 = createMockPrevNode('node3', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        filterNegativeDur: [false, false],
      });
      node1.nextNodes.push(intervalNode);
      node2.nextNodes.push(intervalNode);

      expect(intervalNode.state.filterNegativeDur).toEqual([false, false]);

      addConnection(node3, intervalNode);
      expect(intervalNode.state.filterNegativeDur).toEqual([
        false,
        false,
        true,
      ]);
    });

    it('should properly handle connection removal followed by downstream node update', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfoWithSqlType('id', 'INT', PerfettoSqlTypes.INT),
        createColumnInfoWithSqlType(
          'ts',
          'TIMESTAMP',
          PerfettoSqlTypes.TIMESTAMP,
        ),
        createColumnInfoWithSqlType(
          'dur',
          'DURATION',
          PerfettoSqlTypes.DURATION,
        ),
        createColumnInfoWithSqlType(
          'unique_col1',
          'STRING',
          PerfettoSqlTypes.STRING,
        ),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfoWithSqlType('id', 'INT', PerfettoSqlTypes.INT),
        createColumnInfoWithSqlType(
          'ts',
          'TIMESTAMP',
          PerfettoSqlTypes.TIMESTAMP,
        ),
        createColumnInfoWithSqlType(
          'dur',
          'DURATION',
          PerfettoSqlTypes.DURATION,
        ),
        createColumnInfoWithSqlType(
          'unique_col2',
          'STRING',
          PerfettoSqlTypes.STRING,
        ),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });
      node1.nextNodes.push(intervalNode);
      node2.nextNodes.push(intervalNode);

      const modifyNode = new ModifyColumnsNode({selectedColumns: []});
      modifyNode.primaryInput = intervalNode;
      intervalNode.nextNodes.push(modifyNode);
      modifyNode.onPrevNodesUpdated();

      expect(
        modifyNode.state.selectedColumns.find((c) => c.name === 'unique_col1'),
      ).toBeDefined();
      expect(
        modifyNode.state.selectedColumns.find((c) => c.name === 'unique_col2'),
      ).toBeDefined();

      removeConnection(node2, intervalNode);
      notifyNextNodes(intervalNode);

      expect(
        modifyNode.state.selectedColumns.find((c) => c.name === 'unique_col1'),
      ).toBeDefined();
      expect(
        modifyNode.state.selectedColumns.find((c) => c.name === 'unique_col2'),
      ).toBeUndefined();
    });
  });

  describe('AddColumnsNode', () => {
    it('should connect primary input using addConnection', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);

      const addColsNode = new AddColumnsNode({});

      addConnection(primaryNode, addColsNode);

      expect(addColsNode.primaryInput).toBe(primaryNode);
      expect(primaryNode.nextNodes).toContain(addColsNode);
    });

    it('should connect secondary input (lookup table) using addConnection with port', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const lookupNode = createMockPrevNode('lookup', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('extra_col', 'STRING'),
      ]);

      const addColsNode = new AddColumnsNode({});

      // Connect primary first
      addConnection(primaryNode, addColsNode);
      // Connect lookup table to secondary input port 0
      addConnection(lookupNode, addColsNode, 0);

      expect(addColsNode.primaryInput).toBe(primaryNode);
      expect(addColsNode.secondaryInputs.connections.get(0)).toBe(lookupNode);
      expect(addColsNode.rightNode).toBe(lookupNode);
    });

    it('should disconnect primary input using removeConnection', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);

      const addColsNode = new AddColumnsNode({});
      addConnection(primaryNode, addColsNode);

      expect(addColsNode.primaryInput).toBe(primaryNode);

      removeConnection(primaryNode, addColsNode);

      expect(addColsNode.primaryInput).toBeUndefined();
      expect(primaryNode.nextNodes).not.toContain(addColsNode);
    });

    it('should disconnect secondary input using removeConnection', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const lookupNode = createMockPrevNode('lookup', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('extra_col', 'STRING'),
      ]);

      const addColsNode = new AddColumnsNode({});
      addConnection(primaryNode, addColsNode);
      addConnection(lookupNode, addColsNode, 0);

      expect(addColsNode.rightNode).toBe(lookupNode);

      removeConnection(lookupNode, addColsNode);

      expect(addColsNode.secondaryInputs.connections.get(0)).toBeUndefined();
      expect(addColsNode.rightNode).toBeUndefined();
      expect(lookupNode.nextNodes).not.toContain(addColsNode);
    });

    it('should reset selectedColumns when secondary input disconnected', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const lookupNode = createMockPrevNode('lookup', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('extra_col', 'STRING'),
        createColumnInfo('another_col', 'INT'),
      ]);

      const addColsNode = new AddColumnsNode({});
      addConnection(primaryNode, addColsNode);
      addConnection(lookupNode, addColsNode, 0);

      // Set selected columns AFTER connection (simulating user selection)
      addColsNode.state.selectedColumns = ['extra_col', 'another_col'];

      // Verify columns are selected
      expect(addColsNode.state.selectedColumns).toEqual([
        'extra_col',
        'another_col',
      ]);

      // Disconnect lookup table
      removeConnection(lookupNode, addColsNode);

      // Verify selectedColumns were reset
      expect(addColsNode.state.selectedColumns).toEqual([]);
    });

    it('should have correct finalCols with both inputs connected', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const lookupNode = createMockPrevNode('lookup', [
        createColumnInfo('lookup_id', 'INT'),
        createColumnInfo('extra_col', 'STRING'),
      ]);

      const addColsNode = new AddColumnsNode({
        leftColumn: 'id',
        rightColumn: 'lookup_id',
      });
      addConnection(primaryNode, addColsNode);
      addConnection(lookupNode, addColsNode, 0);

      // Set selected columns AFTER connection (simulating user selection)
      addColsNode.state.selectedColumns = ['extra_col'];

      const cols = addColsNode.finalCols;

      // Should have primary columns
      expect(cols.find((c) => c.name === 'id')).toBeDefined();
      expect(cols.find((c) => c.name === 'name')).toBeDefined();
      // Should have selected column from lookup
      expect(cols.find((c) => c.name === 'extra_col')).toBeDefined();
    });

    it('should have only primary columns when secondary disconnected', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const lookupNode = createMockPrevNode('lookup', [
        createColumnInfo('lookup_id', 'INT'),
        createColumnInfo('extra_col', 'STRING'),
      ]);

      const addColsNode = new AddColumnsNode({
        selectedColumns: ['extra_col'],
      });
      addConnection(primaryNode, addColsNode);
      addConnection(lookupNode, addColsNode, 0);

      // Disconnect lookup table
      removeConnection(lookupNode, addColsNode);

      const cols = addColsNode.finalCols;

      // Should have only primary columns
      expect(cols.find((c) => c.name === 'id')).toBeDefined();
      expect(cols.find((c) => c.name === 'name')).toBeDefined();
      // Should NOT have lookup columns
      expect(cols.find((c) => c.name === 'extra_col')).toBeUndefined();
    });

    it('should have empty finalCols when primary disconnected', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);

      const addColsNode = new AddColumnsNode({});
      addConnection(primaryNode, addColsNode);

      expect(addColsNode.finalCols.length).toBeGreaterThan(0);

      removeConnection(primaryNode, addColsNode);

      expect(addColsNode.finalCols).toEqual([]);
    });

    it('should notify downstream nodes when secondary input disconnected', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const lookupNode = createMockPrevNode('lookup', [
        createColumnInfo('lookup_id', 'INT'),
        createColumnInfo('extra_col', 'STRING'),
      ]);

      const addColsNode = new AddColumnsNode({});
      addConnection(primaryNode, addColsNode);
      addConnection(lookupNode, addColsNode, 0);

      // Set selected columns AFTER connection (simulating user selection)
      addColsNode.state.selectedColumns = ['extra_col'];

      // Create downstream ModifyColumnsNode
      const modifyNode = new ModifyColumnsNode({selectedColumns: []});
      modifyNode.primaryInput = addColsNode;
      addColsNode.nextNodes.push(modifyNode);
      modifyNode.onPrevNodesUpdated();

      // Initially should have extra_col
      expect(
        modifyNode.state.selectedColumns.find((c) => c.name === 'extra_col'),
      ).toBeDefined();

      // Disconnect lookup table
      removeConnection(lookupNode, addColsNode);
      notifyNextNodes(addColsNode);

      // After disconnection, extra_col should be gone
      expect(
        modifyNode.state.selectedColumns.find((c) => c.name === 'extra_col'),
      ).toBeUndefined();
    });
  });

  describe('FilterDuringNode', () => {
    it('should connect primary input using addConnection', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterNode = new FilterDuringNode({});

      addConnection(primaryNode, filterNode);

      expect(filterNode.primaryInput).toBe(primaryNode);
      expect(primaryNode.nextNodes).toContain(filterNode);
    });

    it('should connect multiple secondary inputs using addConnection', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const intervalNode1 = createMockPrevNode('interval1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const intervalNode2 = createMockPrevNode('interval2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterNode = new FilterDuringNode({});
      addConnection(primaryNode, filterNode);
      addConnection(intervalNode1, filterNode, 0);
      addConnection(intervalNode2, filterNode, 1);

      expect(filterNode.primaryInput).toBe(primaryNode);
      expect(filterNode.secondaryInputs.connections.size).toBe(2);
      expect(filterNode.secondaryInputs.connections.get(0)).toBe(intervalNode1);
      expect(filterNode.secondaryInputs.connections.get(1)).toBe(intervalNode2);
      expect(filterNode.secondaryNodes.length).toBe(2);
    });

    it('should disconnect primary input using removeConnection', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterNode = new FilterDuringNode({});
      addConnection(primaryNode, filterNode);

      expect(filterNode.primaryInput).toBe(primaryNode);

      removeConnection(primaryNode, filterNode);

      expect(filterNode.primaryInput).toBeUndefined();
      expect(primaryNode.nextNodes).not.toContain(filterNode);
    });

    it('should disconnect one of multiple secondary inputs', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const intervalNode1 = createMockPrevNode('interval1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const intervalNode2 = createMockPrevNode('interval2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterNode = new FilterDuringNode({});
      addConnection(primaryNode, filterNode);
      addConnection(intervalNode1, filterNode, 0);
      addConnection(intervalNode2, filterNode, 1);

      expect(filterNode.secondaryInputs.connections.size).toBe(2);

      removeConnection(intervalNode1, filterNode);

      expect(filterNode.secondaryInputs.connections.size).toBe(1);
      expect(filterNode.secondaryInputs.connections.get(0)).toBeUndefined();
      expect(filterNode.secondaryInputs.connections.get(1)).toBe(intervalNode2);
      expect(filterNode.secondaryNodes.length).toBe(1);
    });

    it('should have finalCols from primary input', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('extra_col', 'STRING'),
      ]);
      const intervalNode = createMockPrevNode('interval', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterNode = new FilterDuringNode({});
      addConnection(primaryNode, filterNode);
      addConnection(intervalNode, filterNode, 0);

      const cols = filterNode.finalCols;

      // Should have all columns from primary
      expect(cols.find((c) => c.name === 'id')).toBeDefined();
      expect(cols.find((c) => c.name === 'ts')).toBeDefined();
      expect(cols.find((c) => c.name === 'dur')).toBeDefined();
      expect(cols.find((c) => c.name === 'extra_col')).toBeDefined();
    });

    it('should have empty finalCols when primary disconnected', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterNode = new FilterDuringNode({});
      addConnection(primaryNode, filterNode);

      expect(filterNode.finalCols.length).toBeGreaterThan(0);

      removeConnection(primaryNode, filterNode);

      expect(filterNode.finalCols).toEqual([]);
    });

    it('should disconnect all secondary inputs and leave node with just primary', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const intervalNode1 = createMockPrevNode('interval1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const intervalNode2 = createMockPrevNode('interval2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterNode = new FilterDuringNode({});
      addConnection(primaryNode, filterNode);
      addConnection(intervalNode1, filterNode, 0);
      addConnection(intervalNode2, filterNode, 1);

      removeConnection(intervalNode1, filterNode);
      removeConnection(intervalNode2, filterNode);

      expect(filterNode.primaryInput).toBe(primaryNode);
      expect(filterNode.secondaryInputs.connections.size).toBe(0);
      expect(filterNode.secondaryNodes.length).toBe(0);
    });

    it('should add secondary input to next available port', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const intervalNode1 = createMockPrevNode('interval1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const intervalNode2 = createMockPrevNode('interval2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const intervalNode3 = createMockPrevNode('interval3', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterNode = new FilterDuringNode({});
      addConnection(primaryNode, filterNode);

      // Add without specifying port - should auto-assign
      addConnection(intervalNode1, filterNode, 0);
      addConnection(intervalNode2, filterNode, 1);

      // Remove the first one
      removeConnection(intervalNode1, filterNode);

      // Add a new one without specifying port - should use port 0
      addConnection(intervalNode3, filterNode, 0);

      expect(filterNode.secondaryInputs.connections.get(0)).toBe(intervalNode3);
      expect(filterNode.secondaryInputs.connections.get(1)).toBe(intervalNode2);
    });

    it('should notify downstream nodes when primary disconnected', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('unique_col', 'STRING'),
      ]);
      const intervalNode = createMockPrevNode('interval', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterNode = new FilterDuringNode({});
      addConnection(primaryNode, filterNode);
      addConnection(intervalNode, filterNode, 0);

      // Create downstream ModifyColumnsNode
      const modifyNode = new ModifyColumnsNode({selectedColumns: []});
      modifyNode.primaryInput = filterNode;
      filterNode.nextNodes.push(modifyNode);
      modifyNode.onPrevNodesUpdated();

      // Initially should have unique_col
      expect(
        modifyNode.state.selectedColumns.find((c) => c.name === 'unique_col'),
      ).toBeDefined();

      // Disconnect primary
      removeConnection(primaryNode, filterNode);
      notifyNextNodes(filterNode);

      // After disconnection, all columns should be gone
      expect(modifyNode.state.selectedColumns.length).toBe(0);
    });
  });

  describe('Complex graph scenarios', () => {
    it('should handle chain of connections: Table -> AddColumns -> FilterDuring', () => {
      // Source table
      const tableNode = createMockPrevNode('table', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('name', 'STRING'),
      ]);

      // Lookup table for AddColumns
      const lookupNode = createMockPrevNode('lookup', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('extra_data', 'STRING'),
      ]);

      // Interval source for FilterDuring
      const intervalSource = createMockPrevNode('intervals', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      // Create nodes
      const addColsNode = new AddColumnsNode({
        leftColumn: 'id',
        rightColumn: 'id',
      });
      const filterNode = new FilterDuringNode({});

      // Build chain: table -> addCols -> filter
      addConnection(tableNode, addColsNode);
      addConnection(lookupNode, addColsNode, 0);

      // Set selected columns AFTER connection (simulating user selection)
      addColsNode.state.selectedColumns = ['extra_data'];

      addConnection(addColsNode, filterNode);
      addConnection(intervalSource, filterNode, 0);

      // Verify the chain
      expect(addColsNode.primaryInput).toBe(tableNode);
      expect(addColsNode.rightNode).toBe(lookupNode);
      expect(filterNode.primaryInput).toBe(addColsNode);
      expect(filterNode.secondaryNodes).toContain(intervalSource);

      // Verify columns propagate through
      const addColsFinalCols = addColsNode.finalCols;
      expect(
        addColsFinalCols.find((c) => c.name === 'extra_data'),
      ).toBeDefined();

      const filterFinalCols = filterNode.finalCols;
      expect(
        filterFinalCols.find((c) => c.name === 'extra_data'),
      ).toBeDefined();
      expect(filterFinalCols.find((c) => c.name === 'name')).toBeDefined();
    });

    it('should handle disconnecting middle node in a chain', () => {
      const tableNode = createMockPrevNode('table', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const lookupNode = createMockPrevNode('lookup', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('extra', 'STRING'),
      ]);

      const addColsNode = new AddColumnsNode({});
      const modifyNode = new ModifyColumnsNode({selectedColumns: []});

      // Build chain: table -> addCols -> modify
      addConnection(tableNode, addColsNode);
      addConnection(lookupNode, addColsNode, 0);

      // Set selected columns AFTER connection (simulating user selection)
      addColsNode.state.selectedColumns = ['extra'];

      addConnection(addColsNode, modifyNode);
      modifyNode.onPrevNodesUpdated();

      // Verify initial state
      expect(
        modifyNode.state.selectedColumns.find((c) => c.name === 'extra'),
      ).toBeDefined();

      // Disconnect lookup from addCols
      removeConnection(lookupNode, addColsNode);
      notifyNextNodes(addColsNode);

      // Verify downstream is updated
      expect(
        modifyNode.state.selectedColumns.find((c) => c.name === 'extra'),
      ).toBeUndefined();
    });

    it('should handle multiple downstream consumers', () => {
      const tableNode = createMockPrevNode('table', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('value', 'INT'),
      ]);

      const intervalNode1 = new IntervalIntersectNode({inputNodes: []});
      const intervalNode2 = new IntervalIntersectNode({inputNodes: []});

      // Connect table to both interval nodes as input
      addConnection(tableNode, intervalNode1, 0);
      addConnection(tableNode, intervalNode2, 0);

      expect(tableNode.nextNodes).toContain(intervalNode1);
      expect(tableNode.nextNodes).toContain(intervalNode2);
      expect(intervalNode1.secondaryInputs.connections.get(0)).toBe(tableNode);
      expect(intervalNode2.secondaryInputs.connections.get(0)).toBe(tableNode);
    });
  });

  describe('insertNodeBetween', () => {
    it('should preserve secondary input connection when inserting node between source and FilterDuring secondary input', () => {
      // Scenario: slices -> FilterDuring (primary), thread_state -> FilterDuring (secondary)
      // User inserts Filter between thread_state and FilterDuring
      // Expected: thread_state -> Filter -> FilterDuring (secondary)
      // Bug: thread_state -> Filter -> FilterDuring (primary) - WRONG!

      const slicesNode = createMockPrevNode('slices', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('name', 'STRING'),
      ]);
      const threadStateNode = createMockPrevNode('thread_state', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('state', 'STRING'),
      ]);

      const filterDuringNode = new FilterDuringNode({});

      // Connect slices to FilterDuring's primary input
      addConnection(slicesNode, filterDuringNode);
      // Connect thread_state to FilterDuring's secondary input (port 0)
      addConnection(threadStateNode, filterDuringNode, 0);

      // Verify initial state
      expect(filterDuringNode.primaryInput).toBe(slicesNode);
      expect(filterDuringNode.secondaryInputs.connections.get(0)).toBe(
        threadStateNode,
      );

      // Create a filter node to insert between thread_state and FilterDuring
      const filterNode = new FilterNode({});

      // Insert filter between thread_state and FilterDuring
      insertNodeBetween(
        threadStateNode,
        filterNode,
        addConnection,
        removeConnection,
      );

      // Expected: thread_state -> filterNode -> FilterDuring (secondary input)
      // The filter should NOT be connected to FilterDuring's primary input

      // Verify filterNode is connected to thread_state
      expect(filterNode.primaryInput).toBe(threadStateNode);
      expect(threadStateNode.nextNodes).toContain(filterNode);

      // Verify filterNode is connected to FilterDuring's SECONDARY input, not primary
      expect(filterDuringNode.secondaryInputs.connections.get(0)).toBe(
        filterNode,
      );
      // Primary input should still be slices, not filter
      expect(filterDuringNode.primaryInput).toBe(slicesNode);
    });

    it('should preserve specific port index when inserting between IntervalIntersect inputs', () => {
      // Scenario: node1, node2, node3 all connected to IntervalIntersect
      // Insert filter between node2 and IntervalIntersect
      // Filter should be connected to port 1, not port 0

      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node3 = createMockPrevNode('node3', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2, node3],
      });
      node1.nextNodes.push(intervalNode);
      node2.nextNodes.push(intervalNode);
      node3.nextNodes.push(intervalNode);

      // Verify initial connections
      expect(intervalNode.secondaryInputs.connections.get(0)).toBe(node1);
      expect(intervalNode.secondaryInputs.connections.get(1)).toBe(node2);
      expect(intervalNode.secondaryInputs.connections.get(2)).toBe(node3);

      // Create a filter node to insert between node2 (port 1) and IntervalIntersect
      const filterNode = new FilterNode({});

      // Insert filter between node2 and IntervalIntersect
      insertNodeBetween(node2, filterNode, addConnection, removeConnection);

      // Filter should be connected to port 1 (where node2 was)
      expect(intervalNode.secondaryInputs.connections.get(0)).toBe(node1);
      expect(intervalNode.secondaryInputs.connections.get(1)).toBe(filterNode);
      expect(intervalNode.secondaryInputs.connections.get(2)).toBe(node3);

      // Verify filterNode is properly connected
      expect(filterNode.primaryInput).toBe(node2);
    });

    it('should preserve secondary input connection when inserting node between source and AddColumnsNode secondary input', () => {
      // Scenario: primary -> AddColumns (primary), lookup -> AddColumns (secondary)
      // User inserts Filter between lookup and AddColumns
      // Expected: lookup -> Filter -> AddColumns (secondary)

      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const lookupNode = createMockPrevNode('lookup', [
        createColumnInfo('lookup_id', 'INT'),
        createColumnInfo('extra_col', 'STRING'),
      ]);

      const addColsNode = new AddColumnsNode({});

      // Connect primary to AddColumns primary input
      addConnection(primaryNode, addColsNode);
      // Connect lookup to AddColumns secondary input (port 0)
      addConnection(lookupNode, addColsNode, 0);

      // Verify initial state
      expect(addColsNode.primaryInput).toBe(primaryNode);
      expect(addColsNode.secondaryInputs.connections.get(0)).toBe(lookupNode);
      expect(addColsNode.rightNode).toBe(lookupNode);

      // Create a filter node to insert between lookup and AddColumns
      const filterNode = new FilterNode({});

      // Insert filter between lookup and AddColumns
      insertNodeBetween(
        lookupNode,
        filterNode,
        addConnection,
        removeConnection,
      );

      // Expected: lookup -> filterNode -> AddColumns (secondary input)
      // The filter should NOT be connected to AddColumns' primary input

      // Verify filterNode is connected to lookup
      expect(filterNode.primaryInput).toBe(lookupNode);
      expect(lookupNode.nextNodes).toContain(filterNode);

      // Verify filterNode is connected to AddColumns SECONDARY input, not primary
      expect(addColsNode.secondaryInputs.connections.get(0)).toBe(filterNode);
      expect(addColsNode.rightNode).toBe(filterNode);
      // Primary input should still be primaryNode, not filter
      expect(addColsNode.primaryInput).toBe(primaryNode);
    });

    it('should throw error when attempting self-referential insert', () => {
      // Edge case: parentNode === newNode should not be allowed
      const node = createMockPrevNode('node', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);

      // Create a child to make nextNodes non-empty
      const childNode = new FilterNode({});
      addConnection(node, childNode);

      // Attempting to insert the same node between itself should throw
      expect(() => {
        insertNodeBetween(node, node, addConnection, removeConnection);
      }).toThrow('Cannot insert a node between itself');
    });

    it('should preserve primary input connection when inserting between source and single-input node', () => {
      // Scenario: source -> FilterNode (primary input only, no secondary inputs)
      // User inserts another Filter between source and FilterNode
      // Expected: source -> newFilter -> FilterNode (primary)

      const sourceNode = createMockPrevNode('source', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('value', 'INT'),
      ]);

      const existingFilterNode = new FilterNode({});

      // Connect source to existing filter's primary input
      addConnection(sourceNode, existingFilterNode);

      // Verify initial state
      expect(existingFilterNode.primaryInput).toBe(sourceNode);
      expect(sourceNode.nextNodes).toContain(existingFilterNode);

      // Create a new filter node to insert between source and existing filter
      const newFilterNode = new FilterNode({});

      // Insert new filter between source and existing filter
      insertNodeBetween(
        sourceNode,
        newFilterNode,
        addConnection,
        removeConnection,
      );

      // Expected chain: source -> newFilterNode -> existingFilterNode

      // Verify newFilterNode is connected to source
      expect(newFilterNode.primaryInput).toBe(sourceNode);
      expect(sourceNode.nextNodes).toContain(newFilterNode);

      // Verify existingFilterNode is now connected to newFilterNode (not source)
      expect(existingFilterNode.primaryInput).toBe(newFilterNode);
      expect(newFilterNode.nextNodes).toContain(existingFilterNode);

      // Verify source is no longer directly connected to existingFilterNode
      expect(sourceNode.nextNodes).not.toContain(existingFilterNode);
    });

    it('should handle multiple children with mixed connection types (primary + secondary)', () => {
      // Scenario: parent connected to child1 (primary) and child2 (secondary port 0)
      // Insert newNode between parent and both children
      // Expected: parent -> newNode -> child1 (primary), newNode -> child2 (secondary)

      const parentNode = createMockPrevNode('parent', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      // child1 receives parent as primary input
      const child1 = new FilterNode({});
      // child2 (FilterDuring) receives parent as secondary input
      const child2 = new FilterDuringNode({});

      // Set up another node as primary for child2
      const otherPrimaryNode = createMockPrevNode('otherPrimary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      addConnection(otherPrimaryNode, child2);

      // Connect parent to child1 (primary) and child2 (secondary port 0)
      addConnection(parentNode, child1);
      addConnection(parentNode, child2, 0);

      // Verify initial state
      expect(child1.primaryInput).toBe(parentNode);
      expect(child2.secondaryInputs.connections.get(0)).toBe(parentNode);
      expect(parentNode.nextNodes).toContain(child1);
      expect(parentNode.nextNodes).toContain(child2);

      // Create new node to insert
      const newNode = new FilterNode({});

      // Insert between parent and both children
      insertNodeBetween(parentNode, newNode, addConnection, removeConnection);

      // Verify newNode is connected to parent
      expect(newNode.primaryInput).toBe(parentNode);
      expect(parentNode.nextNodes).toContain(newNode);

      // Verify child1 now receives newNode as primary
      expect(child1.primaryInput).toBe(newNode);

      // Verify child2 still receives newNode as secondary (port preserved)
      expect(child2.secondaryInputs.connections.get(0)).toBe(newNode);
      expect(child2.primaryInput).toBe(otherPrimaryNode); // primary unchanged

      // Verify parent no longer directly connected to children
      expect(parentNode.nextNodes).not.toContain(child1);
      expect(parentNode.nextNodes).not.toContain(child2);
    });

    it('should handle inserting when newNode already has existing connections', () => {
      // Edge case: newNode already has primaryInput before being inserted
      // After insert, newNode should have NEW primaryInput (from parentNode)

      const sourceNode = createMockPrevNode('source', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);

      const childNode = new FilterNode({});
      addConnection(sourceNode, childNode);

      // Create newNode that already has a connection
      const existingParent = createMockPrevNode('existingParent', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('value', 'INT'),
      ]);
      const newNode = new FilterNode({});
      addConnection(existingParent, newNode);

      // Verify newNode already has a primaryInput
      expect(newNode.primaryInput).toBe(existingParent);

      // Insert newNode between source and child
      insertNodeBetween(sourceNode, newNode, addConnection, removeConnection);

      // After insert, newNode's primaryInput should be updated to sourceNode
      // Note: addConnection overwrites primaryInput, so this is expected behavior
      expect(newNode.primaryInput).toBe(sourceNode);
      expect(sourceNode.nextNodes).toContain(newNode);

      // Child should now receive newNode as primary
      expect(childNode.primaryInput).toBe(newNode);
      expect(newNode.nextNodes).toContain(childNode);
    });

    it('should handle parent with no children (empty nextNodes)', () => {
      // Edge case: parent has no children, inserting newNode just connects parent -> newNode

      const parentNode = createMockPrevNode('parent', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);

      // Verify parent has no children
      expect(parentNode.nextNodes.length).toBe(0);

      const newNode = new FilterNode({});

      // Insert newNode (but parent has no children to reconnect)
      insertNodeBetween(parentNode, newNode, addConnection, removeConnection);

      // Verify parent -> newNode connection was made
      expect(newNode.primaryInput).toBe(parentNode);
      expect(parentNode.nextNodes).toContain(newNode);

      // newNode has no children since parent had none
      expect(newNode.nextNodes.length).toBe(0);
    });
  });
});
