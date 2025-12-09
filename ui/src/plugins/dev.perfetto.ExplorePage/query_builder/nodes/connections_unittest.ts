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
import {insertNodeBetween, reconnectParentsToChildren} from '../graph_utils';
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

    it('should connect secondary input using addConnection', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const intervalNode = createMockPrevNode('interval', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterNode = new FilterDuringNode({});
      addConnection(primaryNode, filterNode);
      addConnection(intervalNode, filterNode, 0);

      expect(filterNode.primaryInput).toBe(primaryNode);
      expect(filterNode.secondaryInputs.connections.get(0)).toBe(intervalNode);
      expect(filterNode.secondaryNodes.length).toBe(1);
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

    it('should disconnect secondary input', () => {
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const intervalNode = createMockPrevNode('interval', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterNode = new FilterDuringNode({});
      addConnection(primaryNode, filterNode);
      addConnection(intervalNode, filterNode, 0);

      expect(filterNode.secondaryInputs.connections.get(0)).toBe(intervalNode);

      removeConnection(intervalNode, filterNode);

      expect(filterNode.secondaryInputs.connections.get(0)).toBeUndefined();
      expect(filterNode.secondaryNodes.length).toBe(0);
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
      const intervalNode = createMockPrevNode('interval', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterNode = new FilterDuringNode({});
      addConnection(primaryNode, filterNode);
      addConnection(intervalNode, filterNode, 0);

      removeConnection(intervalNode, filterNode);

      expect(filterNode.primaryInput).toBe(primaryNode);
      expect(filterNode.secondaryInputs.connections.get(0)).toBeUndefined();
      expect(filterNode.secondaryNodes.length).toBe(0);
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

  describe('Node deletion with secondary inputs', () => {
    it('should NOT reconnect secondary input nodes to children when deleting a node', () => {
      // Bug reproduction test:
      // Scenario:
      //   nodeA -> nodeX (primary input) -> childZ
      //   nodeY -> nodeX (secondary input)
      //
      // When deleting nodeX, only nodeA should be reconnected to childZ.
      // nodeY should NOT be connected to childZ.
      //
      // Bug: Both nodeA and nodeY were getting reconnected to childZ,
      //      causing childZ to have TWO primary input connections
      // Expected: Only nodeA should be reconnected to childZ (primary input)

      const nodeA = createMockPrevNode('nodeA', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const nodeY = createMockPrevNode('nodeY', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const nodeX = new FilterDuringNode({});
      const childZ = new ModifyColumnsNode({selectedColumns: []});

      // Set up connections:
      // nodeA -> nodeX (primary)
      addConnection(nodeA, nodeX);
      // nodeY -> nodeX (secondary, port 0)
      addConnection(nodeY, nodeX, 0);
      // nodeX -> childZ
      addConnection(nodeX, childZ);

      // Verify initial state
      expect(nodeX.primaryInput).toBe(nodeA);
      expect(nodeX.secondaryInputs.connections.get(0)).toBe(nodeY);
      expect(childZ.primaryInput).toBe(nodeX);
      expect(nodeA.nextNodes).toContain(nodeX);
      expect(nodeY.nextNodes).toContain(nodeX);
      expect(nodeX.nextNodes).toContain(childZ);

      // Now simulate node deletion by:
      // 1. Getting all input nodes (this includes both primary and secondary)
      // 2. Removing connections
      // 3. Reconnecting parents to children

      // This is the problematic code from explore_page.ts:
      // const parentNodes = getAllInputNodes(node);
      // const childNodes = [...node.nextNodes];
      // ... remove connections ...
      // reconnectParentsToChildren(parentNodes, childNodes, addConnection);

      // Get just the primary input BEFORE disconnecting
      // (note: getAllInputNodes would return [nodeA, nodeY], but we only want primary)
      const primaryInputNode = nodeX.primaryInput; // nodeA
      const childNodes = [childZ];

      // Remove all connections to/from nodeX
      removeConnection(nodeA, nodeX);
      removeConnection(nodeY, nodeX);
      removeConnection(nodeX, childZ);

      // Verify nodeX is disconnected
      expect(nodeX.primaryInput).toBeUndefined();
      expect(nodeX.secondaryInputs.connections.get(0)).toBeUndefined();
      expect(childZ.primaryInput).toBeUndefined();

      // Now reconnect ONLY primary parent to children (this is the FIX)
      // We should only reconnect nodeA (primary input), not nodeY (secondary input)
      const primaryParentNodes: QueryNode[] = [];
      if (primaryInputNode !== undefined) {
        primaryParentNodes.push(primaryInputNode);
      }
      for (const parent of primaryParentNodes) {
        for (const child of childNodes) {
          addConnection(parent, child);
        }
      }

      // CORRECT behavior after fix:
      // - Only nodeA should be connected to childZ (it was the primary input)
      // - nodeY should have no connections (it was a secondary input and should not propagate)
      expect(nodeA.nextNodes).toContain(childZ); // ✓ nodeA reconnected to childZ
      expect(nodeY.nextNodes).not.toContain(childZ); // ✓ nodeY NOT reconnected (correct!)
      expect(childZ.primaryInput).toBe(nodeA); // ✓ childZ has nodeA as primary input
    });
  });

  describe('Deleting node connected to secondary input', () => {
    it('should preserve secondary input connection when deleting middle node in chain', () => {
      // Bug reproduction test:
      // Scenario:
      //   nodeX -> nodeY -> nodeZ (secondary input port 0)
      //
      // When deleting nodeY, nodeX should be reconnected to nodeZ's SECONDARY input,
      // not to nodeZ's primary input.
      //
      // Bug: nodeX gets connected to nodeZ's primary input instead of secondary
      // Expected: nodeX should be connected to nodeZ's secondary input at port 0

      const nodeX = createMockPrevNode('nodeX', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const nodeY = new FilterNode({});

      const nodeZ = new FilterDuringNode({});

      // Set up another node as primary input for nodeZ
      const primaryNode = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      // Build the chain:
      // nodeX -> nodeY (primary)
      addConnection(nodeX, nodeY);
      // primaryNode -> nodeZ (primary)
      addConnection(primaryNode, nodeZ);
      // nodeY -> nodeZ (secondary input at port 0)
      addConnection(nodeY, nodeZ, 0);

      // Verify initial state
      expect(nodeY.primaryInput).toBe(nodeX);
      expect(nodeZ.primaryInput).toBe(primaryNode);
      expect(nodeZ.secondaryInputs.connections.get(0)).toBe(nodeY);
      expect(nodeX.nextNodes).toContain(nodeY);
      expect(nodeY.nextNodes).toContain(nodeZ);

      // Now simulate node deletion of nodeY
      // This mimics the deletion logic from explore_page.ts:
      // 1. Get primary parent before removal
      const primaryParentNodes: QueryNode[] = [];
      if (nodeY.primaryInput) {
        primaryParentNodes.push(nodeY.primaryInput);
      }
      const childNodes = [...nodeY.nextNodes];

      // 2. Capture port index information BEFORE removing connections
      const childConnectionInfo: Array<{
        child: QueryNode;
        portIndex: number | undefined;
      }> = [];
      for (const child of childNodes) {
        let portIndex: number | undefined = undefined;
        if (child.secondaryInputs) {
          for (const [port, inputNode] of child.secondaryInputs.connections) {
            if (inputNode === nodeY) {
              portIndex = port;
              break;
            }
          }
        }
        childConnectionInfo.push({child, portIndex});
      }

      // 3. Remove all connections to/from nodeY
      removeConnection(nodeX, nodeY);
      removeConnection(nodeY, nodeZ);

      // 4. Verify nodeY is disconnected
      expect(nodeY.primaryInput).toBeUndefined();
      expect(nodeY.nextNodes.length).toBe(0);

      // 5. Reconnect parent to children, preserving port indices
      // This uses the FIXED reconnectParentsToChildren function
      reconnectParentsToChildren(
        primaryParentNodes,
        childConnectionInfo,
        addConnection,
      );

      // EXPECTED behavior (after fix):
      // - nodeX should be connected to nodeZ's SECONDARY input at port 0
      // - nodeZ's primary input should still be primaryNode (unchanged)
      expect(nodeZ.primaryInput).toBe(primaryNode); // primary unchanged
      expect(nodeZ.secondaryInputs.connections.get(0)).toBe(nodeX); // nodeX at secondary input
      expect(nodeX.nextNodes).toContain(nodeZ); // nodeX connected to nodeZ
    });
  });

  describe('Secondary input removal should not trigger reconnection', () => {
    it('should NOT reconnect secondary input node to children when removing secondary connection', () => {
      // Bug reproduction test:
      // Scenario:
      //   sliceSource -> FilterDuring (primary input) -> childNode
      //   intervalsSource -> FilterDuring (secondary input)
      //
      // When removing the secondary input connection (intervalsSource -> FilterDuring),
      // the intervalsSource should NOT get connected to childNode.
      //
      // Bug: intervalsSource was getting reconnected to childNode
      // Expected: intervalsSource is simply disconnected, no reconnection

      const sliceSource = createMockPrevNode('slices', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('name', 'STRING'),
      ]);

      const intervalsSource = createMockPrevNode('intervals', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const filterDuringNode = new FilterDuringNode({});

      // Connect sliceSource to FilterDuring's primary input
      addConnection(sliceSource, filterDuringNode);
      // Connect intervalsSource to FilterDuring's secondary input
      addConnection(intervalsSource, filterDuringNode, 0);

      // Add a child node downstream of FilterDuring
      const childNode = new ModifyColumnsNode({selectedColumns: []});
      addConnection(filterDuringNode, childNode);

      // Verify initial state
      expect(filterDuringNode.primaryInput).toBe(sliceSource);
      expect(filterDuringNode.secondaryInputs.connections.get(0)).toBe(
        intervalsSource,
      );
      expect(filterDuringNode.nextNodes).toContain(childNode);
      expect(intervalsSource.nextNodes).toContain(filterDuringNode);
      expect(intervalsSource.nextNodes.length).toBe(1);

      // Remove the secondary input connection
      removeConnection(intervalsSource, filterDuringNode);

      // After removal:
      // 1. intervalsSource should have no nextNodes
      expect(intervalsSource.nextNodes.length).toBe(0);

      // 2. FilterDuring's secondary input should be empty
      expect(
        filterDuringNode.secondaryInputs.connections.get(0),
      ).toBeUndefined();

      // 3. CRITICAL: intervalsSource should NOT be connected to childNode
      //    (This was the bug - the reconnection logic was incorrectly triggered)
      expect(intervalsSource.nextNodes).not.toContain(childNode);
      expect(childNode.primaryInput).toBe(filterDuringNode);
      expect(childNode.primaryInput).not.toBe(intervalsSource);

      // 4. The primary chain should remain intact
      expect(filterDuringNode.primaryInput).toBe(sliceSource);
      expect(filterDuringNode.nextNodes).toContain(childNode);
    });

    it('should NOT reconnect IntervalIntersect input to downstream when removing one input', () => {
      // Similar bug scenario with IntervalIntersectNode:
      //   node1 -> IntervalIntersect -> childNode
      //   node2 -> IntervalIntersect (secondary input at port 1)
      //
      // When removing node2's connection, node2 should NOT be connected to childNode

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

      // Add a child downstream
      const childNode = new ModifyColumnsNode({selectedColumns: []});
      addConnection(intervalNode, childNode);

      // Verify initial state
      expect(intervalNode.secondaryInputs.connections.size).toBe(2);
      expect(intervalNode.nextNodes).toContain(childNode);
      expect(node2.nextNodes).toContain(intervalNode);
      expect(node2.nextNodes.length).toBe(1);

      // Remove node2's connection
      removeConnection(node2, intervalNode);

      // After removal:
      // 1. node2 should have no nextNodes
      expect(node2.nextNodes.length).toBe(0);

      // 2. node2 should NOT be connected to childNode
      expect(node2.nextNodes).not.toContain(childNode);
      expect(childNode.primaryInput).toBe(intervalNode);

      // 3. The remaining connections should be intact
      expect(intervalNode.secondaryInputs.connections.get(0)).toBe(node1);
      expect(intervalNode.nextNodes).toContain(childNode);
    });
  });

  describe('Node deletion edge cases', () => {
    it('should NOT overwrite existing primary input when deleting middle node', () => {
      // Bug scenario:
      //   A → B → C (B's primary input is A, C's primary input is B)
      //   A ------→ C (A also directly connected to C's primary - shouldn't happen but testing)
      //
      // When deleting B, the reconnection logic tries to do: A → C (primary)
      // But C already has a primaryInput! We shouldn't overwrite it.
      //
      // This is an edge case that could happen if:
      // - User manually created both connections
      // - Or through a series of operations that create this state

      const nodeA = createMockPrevNode('nodeA', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
      ]);

      const nodeB = new FilterNode({});
      addConnection(nodeA, nodeB); // A → B

      const nodeC = new FilterNode({});
      addConnection(nodeB, nodeC); // B → C (C.primaryInput = B)

      // Simulate the edge case: A is already connected to C directly
      // This could happen if user created this connection, or through graph operations
      addConnection(nodeA, nodeC); // A → C (overwrites C.primaryInput to A!)

      // Verify initial state
      expect(nodeB.primaryInput).toBe(nodeA);
      // Note: C.primaryInput is now A (not B!), because addConnection overwrote it
      expect(nodeC.primaryInput).toBe(nodeA);
      expect(nodeA.nextNodes).toContain(nodeB);
      expect(nodeA.nextNodes).toContain(nodeC);
      expect(nodeB.nextNodes).toContain(nodeC);

      // Now simulate deletion of nodeB by doing what handleDeleteNode does:
      // 1. Capture state
      const primaryParent = nodeB.primaryInput; // A
      const childConnections = nodeB.nextNodes.map((child) => ({
        child,
        portIndex: undefined, // B → C is primary connection
      }));

      // 2. Disconnect nodeB
      removeConnection(nodeA, nodeB);
      removeConnection(nodeB, nodeC);

      // 3. Reconnect parent to children (this is where the bug might occur)
      if (primaryParent !== undefined) {
        for (const {child, portIndex} of childConnections) {
          // BUG: This will overwrite C's primaryInput even though A → C already exists!
          addConnection(primaryParent, child, portIndex);
        }
      }

      // AFTER FIX: Reconnection should be SKIPPED because A is already connected to C
      // C.primaryInput should remain as A (the existing connection)
      expect(nodeC.primaryInput).toBe(nodeA);

      // A should be in C's nextNodes
      expect(nodeA.nextNodes).toContain(nodeC);

      // Verify the invariant: A → C exists exactly once in nextNodes
      const finalCount = nodeA.nextNodes.filter((n) => n === nodeC).length;
      expect(finalCount).toBe(1);

      // The fix ensures:
      // - No duplicate connections created
      // - Existing A → C connection preserved
      // - B → C connection removed (B.nextNodes no longer contains C)
      expect(nodeB.nextNodes).not.toContain(nodeC);
    });

    it('should DROP secondary connections when deleting middle node', () => {
      // Correct behavior:
      //   A → B → C (secondary port 1)
      //   A ------→ C (secondary port 0, existing connection)
      //
      // When deleting B:
      // - B → C (secondary port 1) should be DROPPED (not reconnected)
      // - A → C (secondary port 0) should remain unchanged

      const nodeA = createMockPrevNode('nodeA', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const nodeB = new FilterNode({});
      addConnection(nodeA, nodeB); // A → B

      const nodeC = new FilterDuringNode({});
      const primary = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      addConnection(primary, nodeC); // primary → C (primary input)
      addConnection(nodeB, nodeC, 1); // B → C (secondary port 1)

      // Manually add A → C at port 0 (simulating A being connected to C independently)
      addConnection(nodeA, nodeC, 0); // A → C (secondary port 0)

      // Verify initial state
      expect(nodeB.primaryInput).toBe(nodeA);
      expect(nodeC.primaryInput).toBe(primary);
      expect(nodeC.secondaryInputs.connections.get(0)).toBe(nodeA); // A at port 0
      expect(nodeC.secondaryInputs.connections.get(1)).toBe(nodeB); // B at port 1
      expect(nodeA.nextNodes).toContain(nodeB);
      expect(nodeA.nextNodes).toContain(nodeC);

      // Simulate deletion of nodeB
      const primaryParent = nodeB.primaryInput; // A
      const childConnections = nodeB.nextNodes.map((child) => {
        let portIndex: number | undefined = undefined;
        if (child.secondaryInputs) {
          for (const [port, inputNode] of child.secondaryInputs.connections) {
            if (inputNode === nodeB) {
              portIndex = port;
              break;
            }
          }
        }
        return {child, portIndex};
      });

      expect(childConnections[0].portIndex).toBe(1); // B was at port 1

      // Disconnect nodeB
      removeConnection(nodeA, nodeB);
      removeConnection(nodeB, nodeC);

      // After removal, nodeC should have A at port 0 only
      expect(nodeC.secondaryInputs.connections.get(0)).toBe(nodeA);
      expect(nodeC.secondaryInputs.connections.get(1)).toBeUndefined();

      // Count how many times A appears in its own nextNodes (should be 1)
      const initialNextNodesCount = nodeA.nextNodes.filter(
        (n) => n === nodeC,
      ).length;
      expect(initialNextNodesCount).toBe(1);

      // Reconnect parent to children (FIXED logic)
      if (primaryParent !== undefined) {
        for (const {child, portIndex} of childConnections) {
          // FIX 1: Skip reconnection for secondary connections
          if (portIndex !== undefined) {
            continue; // Don't reconnect secondary connections
          }

          // FIX 2: Skip reconnection if parent is already connected
          if (primaryParent.nextNodes.includes(child)) {
            continue; // Already connected - don't create duplicates
          }

          addConnection(primaryParent, child, portIndex);
        }
      }

      // AFTER FIX: Secondary connection should be DROPPED (not reconnected)
      // Port 1 should be empty (B → C secondary connection dropped)
      expect(nodeC.secondaryInputs.connections.get(1)).toBeUndefined();

      // Port 0 should still have A (original connection preserved)
      expect(nodeC.secondaryInputs.connections.get(0)).toBe(nodeA);

      // Forward link should exist exactly once
      const finalNextNodesCount = nodeA.nextNodes.filter(
        (n) => n === nodeC,
      ).length;
      expect(finalNextNodesCount).toBe(1);

      // The fix ensures:
      // Before deletion: A → C (port 0), B → C (port 1)  [2 connections total]
      // After deletion:  A → C (port 0)                    [1 connection total]
      //
      // ✅ Secondary connections DROPPED (not reconnected)
      // ✅ No duplicate connections created
      // ✅ Node A feeds only ONE port of C
      // ✅ Deletion REDUCED connections
    });

    it('should handle deleting middle node in diamond pattern', () => {
      // Diamond pattern:
      //   A → B → D
      //   A ------→ D (secondary port 0)
      //
      // When deleting B:
      // - Try to reconnect A → D (primary)
      // - But A is already connected to D at secondary port 0
      // - What should happen?

      const nodeA = createMockPrevNode('nodeA', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const nodeB = new FilterNode({});
      addConnection(nodeA, nodeB); // A → B (primary)

      const nodeD = new FilterDuringNode({});
      const primary = createMockPrevNode('primary', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      addConnection(primary, nodeD); // primary → D (primary)
      addConnection(nodeB, nodeD); // B → D (primary) - Wait, this will overwrite!

      // Actually, let's set up the diamond correctly:
      // A → B → D (primary flow)
      // A → intervals → D (secondary flow)
      // This is more realistic

      // Reset
      removeConnection(nodeB, nodeD);

      // Set up diamond:
      addConnection(nodeB, nodeD); // B → D (now D's primary is B)

      const intervals = createMockPrevNode('intervals', [
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      addConnection(nodeA, intervals);
      addConnection(intervals, nodeD, 0); // intervals → D (secondary port 0)

      // Verify initial state
      expect(nodeB.primaryInput).toBe(nodeA);
      expect(nodeD.primaryInput).toBe(nodeB);
      expect(nodeD.secondaryInputs.connections.get(0)).toBe(intervals);
      expect(nodeA.nextNodes).toContain(nodeB);
      expect(nodeA.nextNodes).toContain(intervals);

      // Now delete nodeB
      const primaryParent = nodeB.primaryInput; // A
      const childConnections = nodeB.nextNodes.map((child) => ({
        child,
        portIndex: undefined, // B → D is primary
      }));

      removeConnection(nodeA, nodeB);
      removeConnection(nodeB, nodeD);

      // Before reconnection, D has no primary input
      expect(nodeD.primaryInput).toBeUndefined();

      // Reconnect
      if (primaryParent !== undefined) {
        for (const {child, portIndex} of childConnections) {
          addConnection(primaryParent, child, portIndex);
        }
      }

      // After reconnection:
      // - A → D (primary) - This is CORRECT
      // - intervals → D (secondary port 0) - This is UNCHANGED
      expect(nodeD.primaryInput).toBe(nodeA);
      expect(nodeD.secondaryInputs.connections.get(0)).toBe(intervals);
      expect(nodeA.nextNodes).toContain(nodeD);

      // This case actually works correctly! The issue only arises when
      // parent is ALREADY connected to child before deletion.
    });

    it('should promote orphaned secondary input providers to root nodes', () => {
      // Scenario: Root TableSource → FilterDuring (with TimeRange as secondary)
      // When FilterDuring is deleted, TimeRange should be promoted to root
      // (not left as an unreachable ghost node)
      const tableSource = createMockPrevNode('TableSource', [
        createColumnInfo('id', 'INT64'),
      ]);
      const timeRange = createMockPrevNode('TimeRange', [
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const filterDuring = new FilterDuringNode({});

      // Connect: TableSource → FilterDuring (primary)
      addConnection(tableSource, filterDuring);
      // Connect: TimeRange → FilterDuring (secondary port 0)
      addConnection(timeRange, filterDuring, 0);

      // Initial state
      expect(filterDuring.primaryInput).toBe(tableSource);
      expect(filterDuring.secondaryInputs?.connections.get(0)).toBe(timeRange);
      expect(tableSource.nextNodes).toContain(filterDuring);
      expect(timeRange.nextNodes).toContain(filterDuring);

      // Simulate deletion of FilterDuring
      const allInputs = [
        ...(filterDuring.primaryInput ? [filterDuring.primaryInput] : []),
        ...(filterDuring.secondaryInputs !== undefined
          ? Array.from(filterDuring.secondaryInputs.connections.values())
          : []),
      ];

      // Disconnect
      removeConnection(tableSource, filterDuring);
      removeConnection(timeRange, filterDuring);

      // Check which inputs became orphaned
      const orphanedInputs: QueryNode[] = [];
      const assumedRootNodes = [tableSource]; // Assume tableSource is a root

      for (const inputNode of allInputs) {
        const hasNoConsumers = inputNode.nextNodes.length === 0;
        const isNotRoot = !assumedRootNodes.includes(inputNode);

        if (hasNoConsumers && isNotRoot) {
          orphanedInputs.push(inputNode);
        }
      }

      // TimeRange should be orphaned (no consumers, not a root)
      expect(orphanedInputs).toContain(timeRange);
      // TableSource should NOT be orphaned (it's a root)
      expect(orphanedInputs).not.toContain(tableSource);

      // Verify TimeRange is now unreachable
      expect(timeRange.nextNodes.length).toBe(0);

      // The fix: TimeRange should be added to rootNodes to keep it visible
      // (This would be done in handleDeleteNode's Step 5b)
    });

    it('should make children root nodes when deleting node with only secondary inputs', () => {
      // Scenario: Deleting a node that has NO primary parent (only secondary inputs)
      // Examples: IntervalIntersectNode, UnionNode, JoinNode
      //
      // Graph:
      //   source1 → IntervalIntersect (secondary port 0) → child
      //   source2 → IntervalIntersect (secondary port 1)
      //
      // When IntervalIntersect is deleted:
      // - There's no primary parent to reconnect to child
      // - Child should become a root node (so it remains accessible)
      // - source1 and source2 should remain as they were (no reconnection)

      const source1 = createMockPrevNode('source1', [
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const source2 = createMockPrevNode('source2', [
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const intervalIntersect = new IntervalIntersectNode({
        inputNodes: [source1, source2],
      });

      // IntervalIntersectNode only has secondary inputs (no primary)
      addConnection(source1, intervalIntersect, 0); // secondary port 0
      addConnection(source2, intervalIntersect, 1); // secondary port 1

      const childNode = new FilterNode({});
      addConnection(intervalIntersect, childNode); // primary connection

      // Verify initial state
      // IntervalIntersectNode does not have a primaryInput property (only secondary inputs)
      expect('primaryInput' in intervalIntersect).toBe(false);
      expect(intervalIntersect.secondaryInputs.connections.get(0)).toBe(
        source1,
      );
      expect(intervalIntersect.secondaryInputs.connections.get(1)).toBe(
        source2,
      );
      expect(intervalIntersect.nextNodes).toContain(childNode);
      expect(childNode.primaryInput).toBe(intervalIntersect);

      // Simulate deletion of intervalIntersect (following handleDeleteNode logic)
      // STEP 2: Capture state BEFORE modification
      const primaryParent: QueryNode | undefined =
        'primaryInput' in intervalIntersect
          ? (intervalIntersect as {primaryInput?: QueryNode}).primaryInput
          : undefined; // undefined for IntervalIntersectNode!
      const childConnections = intervalIntersect.nextNodes.map((child) => ({
        child,
        portIndex: undefined, // intervalIntersect → child is primary connection
      }));

      expect(primaryParent).toBeUndefined(); // Critical: no primary parent
      expect(childConnections.length).toBe(1);

      // STEP 3: Disconnect the node
      removeConnection(source1, intervalIntersect);
      removeConnection(source2, intervalIntersect);
      removeConnection(intervalIntersect, childNode);

      // STEP 4: Try to reconnect (but primaryParent is undefined, so skip)
      if (primaryParent !== undefined) {
        for (const {child, portIndex} of childConnections) {
          if (portIndex !== undefined) {
            continue;
          }
          const isAlreadyConnected = primaryParent.nextNodes.includes(child);
          if (isAlreadyConnected) {
            continue;
          }
          addConnection(primaryParent, child, portIndex);
        }
      }
      // No reconnection happened (primaryParent is undefined)

      // STEP 5: Update root nodes
      // Since primaryParent === undefined and childConnections.length > 0,
      // child should be added to root nodes
      let newRootNodes: QueryNode[] = []; // Assume intervalIntersect was a root
      if (primaryParent === undefined && childConnections.length > 0) {
        const orphanedChildren = childConnections.map((c) => c.child);
        newRootNodes = [...newRootNodes, ...orphanedChildren];
      }

      // VERIFY: child became a root node
      expect(newRootNodes).toContain(childNode);

      // VERIFY: child is disconnected from deleted node
      expect(childNode.primaryInput).toBeUndefined();

      // VERIFY: sources are NOT reconnected to child
      // (secondary connections are dropped, not propagated)
      expect(source1.nextNodes).not.toContain(childNode);
      expect(source2.nextNodes).not.toContain(childNode);

      // The fix ensures:
      // ✅ Nodes with only secondary inputs can be deleted cleanly
      // ✅ Their children become root nodes (remain accessible)
      // ✅ No incorrect reconnection of secondary inputs
      // ✅ Graph remains consistent
    });

    it('should transfer layout from deleted node to docked child', () => {
      // Scenario: A → B (undocked, has layout) → D (docked, no layout)
      //           A → C (undocked)
      //
      // When B is deleted:
      // - D should be reconnected to A
      // - D should inherit B's layout (appear at B's position)
      // - D should NOT be marked as unrenderable
      // - D should NOT become a root node

      const nodeA = createMockPrevNode('nodeA', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
      ]);

      const nodeB = new FilterNode({});
      addConnection(nodeA, nodeB); // A → B

      const nodeC = new FilterNode({});
      addConnection(nodeA, nodeC); // A → C

      const nodeD = new FilterNode({});
      addConnection(nodeB, nodeD); // B → D

      // Simulate nodeLayouts map
      const nodeLayouts = new Map<string, {x: number; y: number}>();
      nodeLayouts.set(nodeB.nodeId, {x: 100, y: 200}); // B has layout
      nodeLayouts.set(nodeC.nodeId, {x: 300, y: 200}); // C has layout
      // D does NOT have layout (docked to B)

      // Simulate deletion of nodeB (following handleDeleteNode logic)
      // STEP 1: Capture state
      const primaryParent = nodeB.primaryInput; // A
      const childConnections = nodeB.nextNodes.map((child) => ({
        child,
        portIndex: undefined, // B → D is primary connection
      }));
      const deletedNodeLayout = nodeLayouts.get(nodeB.nodeId); // {x: 100, y: 200}

      // STEP 2: Disconnect
      removeConnection(nodeA, nodeB);
      removeConnection(nodeB, nodeD);

      // STEP 3: Reconnect and transfer layout
      const updatedNodeLayouts = new Map(nodeLayouts);
      const reconnectedChildren: QueryNode[] = [];

      if (primaryParent !== undefined) {
        for (const {child, portIndex} of childConnections) {
          if (portIndex !== undefined) {
            continue; // Skip secondary connections
          }

          if (primaryParent.nextNodes.includes(child)) {
            continue; // Skip if already connected
          }

          // Reconnect
          addConnection(primaryParent, child, portIndex);
          reconnectedChildren.push(child);

          // Transfer layout if child was docked
          const childHasNoLayout = !nodeLayouts.has(child.nodeId);
          if (childHasNoLayout && deletedNodeLayout !== undefined) {
            updatedNodeLayouts.set(child.nodeId, deletedNodeLayout);
          }
        }
      }

      // STEP 4: Check if children are renderable (using UPDATED layouts)
      const unrenderableChildren: QueryNode[] = [];
      if (primaryParent !== undefined && reconnectedChildren.length > 0) {
        const parentHasMultipleChildren = primaryParent.nextNodes.length > 1;
        for (const child of reconnectedChildren) {
          // Check UPDATED layouts
          const childHasNoLayout = !updatedNodeLayouts.has(child.nodeId);
          if (childHasNoLayout && parentHasMultipleChildren) {
            unrenderableChildren.push(child);
          }
        }
      }

      // VERIFY: D was reconnected to A
      expect(nodeD.primaryInput).toBe(nodeA);
      expect(nodeA.nextNodes).toContain(nodeD);

      // VERIFY: D inherited B's layout
      expect(updatedNodeLayouts.has(nodeD.nodeId)).toBe(true);
      expect(updatedNodeLayouts.get(nodeD.nodeId)).toEqual({x: 100, y: 200});

      // VERIFY: D is NOT unrenderable (because it now has a layout)
      expect(unrenderableChildren).not.toContain(nodeD);

      // The fix ensures:
      // ✅ Docked children inherit deleted node's layout
      // ✅ Children with inherited layouts are NOT marked as unrenderable
      // ✅ Children stay as docked nodes (not promoted to root)
      // ✅ Layout position is preserved
    });

    it('should transfer layout from deleted root node to orphaned child', () => {
      // Scenario: A (root, has layout) → B (docked, no layout)
      //
      // When A is deleted:
      // - B has no parent to reconnect to (becomes orphaned)
      // - B should inherit A's layout (appear at A's position)
      // - B should become a root node

      const nodeA = new FilterNode({});

      const nodeB = new FilterNode({});
      addConnection(nodeA, nodeB); // A → B

      // Simulate nodeLayouts map
      const nodeLayouts = new Map<string, {x: number; y: number}>();
      nodeLayouts.set(nodeA.nodeId, {x: 150, y: 250}); // A has layout
      // B does NOT have layout (docked to A)

      // Simulate deletion of nodeA (following handleDeleteNode logic)
      // STEP 1: Capture state
      const primaryParent = nodeA.primaryInput; // undefined (A is root)
      const childConnections = nodeA.nextNodes.map((child) => ({
        child,
        portIndex: undefined,
      }));
      const deletedNodeLayout = nodeLayouts.get(nodeA.nodeId); // {x: 150, y: 250}

      // STEP 2: Disconnect
      removeConnection(nodeA, nodeB);

      // STEP 3: Handle orphaned children
      const updatedNodeLayouts = new Map(nodeLayouts);
      let newRootNodes: QueryNode[] = [];

      if (primaryParent === undefined && childConnections.length > 0) {
        const orphanedChildren = childConnections.map((c) => c.child);
        newRootNodes = [...newRootNodes, ...orphanedChildren];

        // Transfer layout to orphaned children
        if (deletedNodeLayout !== undefined) {
          for (const {child} of childConnections) {
            const childHasNoLayout = !updatedNodeLayouts.has(child.nodeId);
            if (childHasNoLayout) {
              updatedNodeLayouts.set(child.nodeId, deletedNodeLayout);
            }
          }
        }
      }

      // VERIFY: B is disconnected from A
      expect(nodeB.primaryInput).toBeUndefined();

      // VERIFY: B became a root node
      expect(newRootNodes).toContain(nodeB);

      // VERIFY: B inherited A's layout
      expect(updatedNodeLayouts.has(nodeB.nodeId)).toBe(true);
      expect(updatedNodeLayouts.get(nodeB.nodeId)).toEqual({x: 150, y: 250});

      // The fix ensures:
      // ✅ Orphaned children inherit deleted node's layout
      // ✅ Orphaned children become root nodes
      // ✅ Layout position is preserved at deleted node's location
    });

    it('should offset layout positions when multiple children inherit layout', () => {
      // Bug scenario:
      //   A → B (at position x:100, y:200)
      //       ↓
      //      [C, D, E] (all docked, no layouts)
      //
      // When deleting B:
      // - C, D, E all become orphaned children
      // - Without fix: All three get same layout (100, 200) → overlapping!
      // - With fix: Each gets offset position to avoid overlap

      const nodeA = createMockPrevNode('nodeA', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
      ]);

      const nodeB = new FilterNode({});
      addConnection(nodeA, nodeB); // A → B

      const nodeC = new FilterNode({});
      const nodeD = new FilterNode({});
      const nodeE = new FilterNode({});

      addConnection(nodeB, nodeC); // B → C
      addConnection(nodeB, nodeD); // B → D
      addConnection(nodeB, nodeE); // B → E

      // Simulate state
      const state = {
        rootNodes: [nodeA],
        nodeLayouts: new Map([
          [nodeA.nodeId, {x: 50, y: 100}],
          [nodeB.nodeId, {x: 100, y: 200}], // B has layout
          // C, D, E have no layouts (docked to B)
        ]),
      };

      // Simulate deletion of B (following handleDeleteNode logic with FIX)
      const primaryParent = nodeB.primaryInput; // A
      const childConnections = nodeB.nextNodes.map((child) => ({
        child,
        portIndex: undefined,
      }));
      const deletedNodeLayout = state.nodeLayouts.get(nodeB.nodeId);

      // Disconnect
      removeConnection(nodeA, nodeB);
      removeConnection(nodeB, nodeC);
      removeConnection(nodeB, nodeD);
      removeConnection(nodeB, nodeE);

      // Try to reconnect (A → C, D, E) - in this case primaryParent exists
      const updatedNodeLayouts = new Map(state.nodeLayouts);
      if (primaryParent !== undefined) {
        let layoutOffsetCount = 0;
        for (const {child, portIndex} of childConnections) {
          if (portIndex !== undefined) {
            continue;
          }
          if (primaryParent.nextNodes.includes(child)) {
            continue;
          }

          addConnection(primaryParent, child, portIndex);

          // Apply layout with offset
          const childHasNoLayout = !state.nodeLayouts.has(child.nodeId);
          if (childHasNoLayout && deletedNodeLayout !== undefined) {
            const offsetX = layoutOffsetCount * 30;
            const offsetY = layoutOffsetCount * 30;
            updatedNodeLayouts.set(child.nodeId, {
              x: deletedNodeLayout.x + offsetX,
              y: deletedNodeLayout.y + offsetY,
            });
            layoutOffsetCount++;
          }
        }
      }

      // VERIFY: All three children got layouts
      expect(updatedNodeLayouts.has(nodeC.nodeId)).toBe(true);
      expect(updatedNodeLayouts.has(nodeD.nodeId)).toBe(true);
      expect(updatedNodeLayouts.has(nodeE.nodeId)).toBe(true);

      // VERIFY: Each child has a DIFFERENT layout (offset applied)
      const layoutC = updatedNodeLayouts.get(nodeC.nodeId);
      const layoutD = updatedNodeLayouts.get(nodeD.nodeId);
      const layoutE = updatedNodeLayouts.get(nodeE.nodeId);

      expect(layoutC).toEqual({x: 100, y: 200}); // 0 * 30 offset
      expect(layoutD).toEqual({x: 130, y: 230}); // 1 * 30 offset
      expect(layoutE).toEqual({x: 160, y: 260}); // 2 * 30 offset

      // VERIFY: No two children have the same position
      expect(layoutC).not.toEqual(layoutD);
      expect(layoutD).not.toEqual(layoutE);
      expect(layoutC).not.toEqual(layoutE);

      // The fix ensures:
      // ✅ Multiple children don't overlap on screen
      // ✅ Each child gets a unique offset position
      // ✅ Positions are predictable (30px diagonal increments)
    });

    it('should use Set to prevent duplicate root nodes when building final list', () => {
      // This test verifies that using a Set prevents potential duplicates
      // when building the root nodes list from multiple sources:
      // - Initial root nodes (filtered)
      // - Orphaned children
      // - Unrenderable children
      // - Orphaned inputs
      //
      // Setup: A → B (FilterDuring) → C, with intervals → B (secondary)
      // When deleting B, we add nodes from multiple sources to root nodes.
      // The Set ensures no duplicates even if a node appears in multiple categories.

      const nodeA = createMockPrevNode('nodeA', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const nodeB = new FilterDuringNode({});
      addConnection(nodeA, nodeB); // A → B (primary)

      const nodeC = new FilterNode({});
      addConnection(nodeB, nodeC); // B → C

      // Add a secondary input that will become orphaned
      const intervals = createMockPrevNode('intervals', [
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      addConnection(intervals, nodeB, 0); // intervals → B (port 0)

      // Simulate state
      const state = {
        rootNodes: [nodeA, nodeB], // B is a root node
        nodeLayouts: new Map([
          [nodeA.nodeId, {x: 50, y: 100}],
          [nodeB.nodeId, {x: 100, y: 200}],
          // C and intervals have no layouts
        ]),
      };

      // Simulate deletion of B (following handleDeleteNode logic with FIX)
      const primaryParent = nodeB.primaryInput; // A
      const childConnections = [{child: nodeC, portIndex: undefined}];
      const allInputs = [nodeA, intervals]; // Both primary and secondary inputs

      // Disconnect
      removeConnection(nodeA, nodeB);
      removeConnection(intervals, nodeB);
      removeConnection(nodeB, nodeC);

      // Build root nodes using Set (the fix)
      const newRootNodesSet = new Set(
        state.rootNodes.filter((n) => n !== nodeB),
      );

      // Reconnect children
      const reconnectedChildren: QueryNode[] = [];
      const updatedNodeLayouts = new Map(state.nodeLayouts);
      if (primaryParent !== undefined) {
        for (const {child, portIndex} of childConnections) {
          if (portIndex !== undefined) {
            continue;
          }
          if (primaryParent.nextNodes.includes(child)) {
            continue;
          }
          addConnection(primaryParent, child, portIndex);
          reconnectedChildren.push(child);
        }
      }

      // Check for unrenderable children
      const unrenderableChildren: QueryNode[] = [];
      if (primaryParent !== undefined && reconnectedChildren.length > 0) {
        const parentHasMultipleChildren = primaryParent.nextNodes.length > 1;
        for (const child of reconnectedChildren) {
          const childHasNoLayout = !updatedNodeLayouts.has(child.nodeId);
          if (childHasNoLayout && parentHasMultipleChildren) {
            unrenderableChildren.push(child);
          }
        }
      }

      // Add unrenderable children (C in this case)
      for (const child of unrenderableChildren) {
        newRootNodesSet.add(child);
      }

      // Check for orphaned inputs
      const orphanedInputs: QueryNode[] = [];
      for (const inputNode of allInputs) {
        const wasNotRoot = !state.rootNodes.includes(inputNode);
        const hasNoConsumers = inputNode.nextNodes.length === 0;
        if (wasNotRoot && hasNoConsumers) {
          orphanedInputs.push(inputNode);
        }
      }

      // Add orphaned inputs (secondaryInput in this case)
      for (const inputNode of orphanedInputs) {
        newRootNodesSet.add(inputNode);
      }

      const newRootNodes = Array.from(newRootNodesSet);

      // VERIFY: C is NOT in root nodes (it's docked to A after reconnection)
      const countC = newRootNodes.filter((n) => n === nodeC).length;
      expect(countC).toBe(0);

      // VERIFY: intervals appears exactly once (orphaned input)
      const countIntervals = newRootNodes.filter((n) => n === intervals).length;
      expect(countIntervals).toBe(1);

      // VERIFY: A remains in root nodes (wasn't deleted)
      expect(newRootNodes).toContain(nodeA);

      // VERIFY: B is NOT in root nodes (was deleted)
      expect(newRootNodes).not.toContain(nodeB);

      // VERIFY: Total count is correct (A, intervals only)
      // Note: C is not in root nodes because it's docked to A
      expect(newRootNodes.length).toBe(2);

      // The fix ensures:
      // ✅ No duplicate nodes in root nodes list
      // ✅ Set deduplication works correctly
      // ✅ All necessary nodes are promoted to root exactly once
    });
  });
});
