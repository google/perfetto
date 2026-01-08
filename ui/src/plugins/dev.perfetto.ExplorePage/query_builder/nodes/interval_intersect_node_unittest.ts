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

import {IntervalIntersectNode} from './interval_intersect_node';
import {ModifyColumnsNode} from './modify_columns_node';
import {QueryNode} from '../../query_node';
import {notifyNextNodes} from '../graph_utils';
import {ColumnInfo} from '../column_info';
import {
  PerfettoSqlType,
  PerfettoSqlTypes,
} from '../../../../trace_processor/perfetto_sql_type';
import {createMockNode} from '../testing/test_utils';

describe('IntervalIntersectNode', () => {
  function createMockPrevNode(id: string, columns: ColumnInfo[]): QueryNode {
    return createMockNode({
      nodeId: id,
      columns,
      getTitle: () => `Mock ${id}`,
    });
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

  // Creates a ColumnInfo with full PerfettoSqlType for the column.type field
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

  describe('constructor', () => {
    it('should initialize with default filterNegativeDur as true', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      expect(node.state.filterNegativeDur).toEqual([true, true]);
    });

    it('should preserve provided filterNegativeDur values', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        filterNegativeDur: [false, true],
      });

      expect(node.state.filterNegativeDur).toEqual([false, true]);
    });

    it('should fill missing filterNegativeDur indices with true', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2, node3],
        filterNegativeDur: [false],
      });

      expect(node.state.filterNegativeDur).toEqual([false, true, true]);
    });

    it('should set autoExecute to false by default', () => {
      const node = new IntervalIntersectNode({
        inputNodes: [],
      });

      expect(node.state.autoExecute).toBe(false);
    });
  });

  describe('finalCols', () => {
    it('should return empty array when no prev nodes', () => {
      const node = new IntervalIntersectNode({
        inputNodes: [],
      });

      expect(node.finalCols).toEqual([]);
    });

    it('should include ts and dur from intersection without suffix', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      const cols = node.finalCols;
      expect(cols[0].name).toBe('ts');
      expect(cols[1].name).toBe('dur');
    });

    it('should include partition columns without suffix', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        partitionColumns: ['utid'],
      });

      const cols = node.finalCols;
      expect(cols[0].name).toBe('ts');
      expect(cols[1].name).toBe('dur');
      expect(cols[2].name).toBe('utid');
    });

    it('should include id_N, ts_N, dur_N for each input with correct types', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2, node3],
      });

      const cols = node.finalCols;

      // After ts, dur (indexes 0, 1), we should have id_0, ts_0, dur_0
      expect(cols[2].name).toBe('id_0');
      expect(cols[2].type).toBe('INT');
      expect(cols[3].name).toBe('ts_0');
      expect(cols[3].type).toBe('TIMESTAMP'); // ts columns are TIMESTAMP type
      expect(cols[4].name).toBe('dur_0');
      expect(cols[4].type).toBe('DURATION'); // dur columns are DURATION type

      // Then id_1, ts_1, dur_1
      expect(cols[5].name).toBe('id_1');
      expect(cols[5].type).toBe('INT');
      expect(cols[6].name).toBe('ts_1');
      expect(cols[6].type).toBe('TIMESTAMP');
      expect(cols[7].name).toBe('dur_1');
      expect(cols[7].type).toBe('DURATION');

      // Then id_2, ts_2, dur_2
      expect(cols[8].name).toBe('id_2');
      expect(cols[8].type).toBe('INT');
      expect(cols[9].name).toBe('ts_2');
      expect(cols[9].type).toBe('TIMESTAMP');
      expect(cols[10].name).toBe('dur_2');
      expect(cols[10].type).toBe('DURATION');
    });

    it('should include non-duplicated columns from all inputs', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('value', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('status', 'STRING'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      const cols = node.finalCols;
      const colNames = cols.map((c) => c.name);

      // Should have ts, dur, id_0, ts_0, dur_0, id_1, ts_1, dur_1
      expect(colNames).toContain('ts');
      expect(colNames).toContain('dur');
      expect(colNames).toContain('id_0');
      expect(colNames).toContain('ts_0');
      expect(colNames).toContain('dur_0');
      expect(colNames).toContain('id_1');
      expect(colNames).toContain('ts_1');
      expect(colNames).toContain('dur_1');

      // Should have non-duplicated columns
      expect(colNames).toContain('name');
      expect(colNames).toContain('value');
      expect(colNames).toContain('status');

      // Verify types are preserved
      const nameCol = cols.find((c) => c.name === 'name');
      expect(nameCol?.type).toBe('STRING');
      const valueCol = cols.find((c) => c.name === 'value');
      expect(valueCol?.type).toBe('INT');
      const statusCol = cols.find((c) => c.name === 'status');
      expect(statusCol?.type).toBe('STRING');
    });

    it('should exclude duplicated columns entirely', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('value', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('name', 'STRING'), // Duplicate
        createColumnInfo('status', 'STRING'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      const cols = node.finalCols;
      const colNames = cols.map((c) => c.name);

      // Should NOT include 'name' since it's duplicated
      expect(colNames).not.toContain('name');
      // Should include unique columns
      expect(colNames).toContain('value');
      expect(colNames).toContain('status');
    });

    it('should exclude partition columns from duplicated columns', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
        createColumnInfo('status', 'STRING'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        partitionColumns: ['utid'],
      });

      const cols = node.finalCols;
      const colNames = cols.map((c) => c.name);

      // utid should appear once without suffix (as partition column)
      const utidOccurrences = colNames.filter((n) => n === 'utid');
      expect(utidOccurrences.length).toBe(1);

      // Should have other non-duplicated columns
      expect(colNames).toContain('name');
      expect(colNames).toContain('status');
    });

    it('should handle multiple partition columns', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
        createColumnInfo('upid', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
        createColumnInfo('upid', 'INT'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        partitionColumns: ['utid', 'upid'],
      });

      const cols = node.finalCols;
      expect(cols[0].name).toBe('ts');
      expect(cols[1].name).toBe('dur');
      expect(cols[2].name).toBe('utid');
      expect(cols[3].name).toBe('upid');
    });

    it('should set all columns as checked', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT', false),
        createColumnInfo('ts', 'INT64', false),
        createColumnInfo('dur', 'INT64', false),
        createColumnInfo('name', 'STRING', false),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT', false),
        createColumnInfo('ts', 'INT64', false),
        createColumnInfo('dur', 'INT64', false),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      const cols = node.finalCols;
      // All columns should be checked
      for (const col of cols) {
        expect(col.checked).toBe(true);
      }
    });

    it('should preserve types for all columns including non-duplicated columns', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('value', 'DOUBLE'),
        createColumnInfo('count', 'LONG'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('status', 'STRING'),
        createColumnInfo('priority', 'INT'),
      ]);
      const node3 = createMockPrevNode('node3', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('category', 'STRING'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2, node3],
      });

      const cols = node.finalCols;

      // Check id_N, ts_N, dur_N types for each input
      // ts columns are TIMESTAMP type, dur columns are DURATION type
      const id0 = cols.find((c) => c.name === 'id_0');
      expect(id0?.type).toBe('INT');
      const ts0 = cols.find((c) => c.name === 'ts_0');
      expect(ts0?.type).toBe('TIMESTAMP');
      const dur0 = cols.find((c) => c.name === 'dur_0');
      expect(dur0?.type).toBe('DURATION');

      const id1 = cols.find((c) => c.name === 'id_1');
      expect(id1?.type).toBe('INT');
      const ts1 = cols.find((c) => c.name === 'ts_1');
      expect(ts1?.type).toBe('TIMESTAMP');
      const dur1 = cols.find((c) => c.name === 'dur_1');
      expect(dur1?.type).toBe('DURATION');

      const id2 = cols.find((c) => c.name === 'id_2');
      expect(id2?.type).toBe('INT');
      const ts2 = cols.find((c) => c.name === 'ts_2');
      expect(ts2?.type).toBe('TIMESTAMP');
      const dur2 = cols.find((c) => c.name === 'dur_2');
      expect(dur2?.type).toBe('DURATION');

      // Check non-duplicated columns preserve their types
      const name = cols.find((c) => c.name === 'name');
      expect(name?.type).toBe('STRING');
      const value = cols.find((c) => c.name === 'value');
      expect(value?.type).toBe('DOUBLE');
      const count = cols.find((c) => c.name === 'count');
      expect(count?.type).toBe('LONG');
      const status = cols.find((c) => c.name === 'status');
      expect(status?.type).toBe('STRING');
      const priority = cols.find((c) => c.name === 'priority');
      expect(priority?.type).toBe('INT');
      const category = cols.find((c) => c.name === 'category');
      expect(category?.type).toBe('STRING');
    });

    it('should exclude columns with conflicting types', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('value', 'DOUBLE'),
        createColumnInfo('unique1', 'STRING'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('value', 'INT'), // Different type - duplicated
        createColumnInfo('unique2', 'INT'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      const cols = node.finalCols;
      const colNames = cols.map((c) => c.name);

      // Should NOT include 'value' since it's duplicated (even with different types)
      expect(colNames).not.toContain('value');
      // Should include unique columns
      expect(colNames).toContain('unique1');
      expect(colNames).toContain('unique2');
    });
  });

  describe('validation', () => {
    it('should fail validation with less than two inputs', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1],
      });

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        'requires at least two inputs',
      );
    });

    it('should pass validation with two valid inputs', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      expect(node.validate()).toBe(true);
    });

    it('should fail validation when input is missing required id column', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        'missing required columns',
      );
      expect(node.state.issues?.queryError?.message).toContain('id');
    });

    it('should fail validation when input is missing required ts column', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        'missing required columns',
      );
      expect(node.state.issues?.queryError?.message).toContain('ts');
    });

    it('should fail validation when input is missing required dur column', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        'missing required columns',
      );
      expect(node.state.issues?.queryError?.message).toContain('dur');
    });

    it('should fail validation when prev node validation fails', () => {
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
      node2.validate = () => false;

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain('is invalid');
    });

    it('should clear previous errors when validation starts', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1],
      });

      // First validation should fail
      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError).toBeDefined();

      // Add second node to make it valid
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);
      node.secondaryInputs.connections.set(1, node2);

      // Second validation should pass and clear errors
      expect(node.validate()).toBe(true);
      expect(node.state.issues?.queryError).toBeUndefined();
    });

    it('should fail validation when partition column is missing from an input', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
      ]);
      const node3 = createMockPrevNode('node3', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        // Missing 'utid' column
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2, node3],
        partitionColumns: ['utid'],
      });

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        "Partition column 'utid' is missing from Input 2",
      );
      expect(node.state.issues?.queryError?.message).toContain(
        'remove the partitioning',
      );
    });

    it('should pass validation when all inputs have partition columns', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
      ]);
      const node3 = createMockPrevNode('node3', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2, node3],
        partitionColumns: ['utid'],
      });

      expect(node.validate()).toBe(true);
    });

    it('should fail validation when only some partition columns are missing', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
        createColumnInfo('upid', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
        // Missing 'upid' column
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        partitionColumns: ['utid', 'upid'],
      });

      expect(node.validate()).toBe(false);
      expect(node.state.issues?.queryError?.message).toContain(
        "Partition column 'upid' is missing from Input 1",
      );
    });

    it('should pass validation with empty partition columns array', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('utid', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
      ]);

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        partitionColumns: [],
      });

      expect(node.validate()).toBe(true);
      expect(node.state.issues?.queryError).toBeUndefined();
    });
  });

  describe('getTitle', () => {
    it('should return "Interval Intersect"', () => {
      const node = new IntervalIntersectNode({
        inputNodes: [],
      });

      expect(node.getTitle()).toBe('Interval Intersect');
    });
  });

  describe('clone', () => {
    it('should create a deep copy of the node', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        filterNegativeDur: [true, false],
        partitionColumns: ['utid'],
      });

      const cloned = node.clone() as IntervalIntersectNode;

      expect(cloned).toBeInstanceOf(IntervalIntersectNode);
      expect(cloned.nodeId).not.toBe(node.nodeId);
      expect(cloned.state.filterNegativeDur).toEqual([true, false]);
      expect(cloned.state.partitionColumns).toEqual(['utid']);
    });

    it('should not share state arrays with original', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        filterNegativeDur: [true, false],
        partitionColumns: ['utid'],
      });

      const cloned = node.clone() as IntervalIntersectNode;

      // Modify cloned arrays
      cloned.state.filterNegativeDur![0] = false;
      cloned.state.partitionColumns!.push('upid');

      // Original should not be affected
      expect(node.state.filterNegativeDur).toEqual([true, false]);
      expect(node.state.partitionColumns).toEqual(['utid']);
    });
  });

  describe('serializeState', () => {
    it('should serialize state correctly', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2, node3],
        filterNegativeDur: [true, false, true],
        partitionColumns: ['utid'],
      });

      const serialized = node.serializeState();

      // All input node IDs are now serialized
      expect(serialized.intervalNodes).toEqual(['node1', 'node2', 'node3']);
      expect(serialized.filterNegativeDur).toEqual([true, false, true]);
      expect(serialized.partitionColumns).toEqual(['utid']);
    });

    it('should handle empty partition columns', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      const serialized = node.serializeState();

      // All input node IDs are now serialized
      expect(serialized.intervalNodes).toEqual(['node1', 'node2']);
      expect(serialized.partitionColumns).toBeUndefined();
    });
  });

  describe('deserializeState', () => {
    it('should deserialize state correctly', () => {
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

      const nodes = new Map([
        ['node1', node1],
        ['node2', node2],
        ['node3', node3],
      ]);

      const serialized = {
        // All input node IDs are stored
        intervalNodes: ['node1', 'node2', 'node3'],
        filterNegativeDur: [true, false, true],
        partitionColumns: ['utid'],
      };

      const deserialized = IntervalIntersectNode.deserializeConnections(
        nodes,
        serialized,
      );

      expect(deserialized.inputNodes).toEqual([node1, node2, node3]);
    });

    it('should handle missing nodes gracefully', () => {
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

      const nodes = new Map([
        ['node1', node1],
        ['node2', node2],
      ]);

      const serialized = {
        // Include a missing node ID to test graceful handling
        intervalNodes: ['node1', 'node2', 'node_missing'],
        filterNegativeDur: [true, false, true],
        partitionColumns: ['utid'],
      };

      const deserialized = IntervalIntersectNode.deserializeConnections(
        nodes,
        serialized,
      );

      // Should only include found nodes (node_missing is filtered out)
      expect(deserialized.inputNodes).toEqual([node1, node2]);
    });
  });

  describe('onPrevNodesUpdated', () => {
    it('should initialize filterNegativeDur when undefined', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      // Clear filterNegativeDur
      node.state.filterNegativeDur = undefined;

      node.onPrevNodesUpdated();

      expect(node.state.filterNegativeDur).toBeDefined();
      expect(node.state.filterNegativeDur).toEqual([true, true]);
    });

    it('should compact filterNegativeDur when inputNodes shrinks', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2, node3],
        filterNegativeDur: [true, false, true],
      });

      // Remove one input node
      node.secondaryInputs.connections.delete(2);

      node.onPrevNodesUpdated();

      expect(node.state.filterNegativeDur).toEqual([true, false]);
    });

    it('should expand filterNegativeDur when inputNodes grows', () => {
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

      const node = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        filterNegativeDur: [true, false],
      });

      // Add another input node
      node.secondaryInputs.connections.set(2, node3);

      node.onPrevNodesUpdated();

      expect(node.state.filterNegativeDur).toEqual([true, false, true]);
    });
  });

  describe('ModifyColumnsNode integration', () => {
    it('should pass columns with correct types to ModifyColumnsNode', () => {
      // Create input nodes with proper SQL types
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
        createColumnInfoWithSqlType('name', 'STRING', PerfettoSqlTypes.STRING),
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
          'status',
          'STRING',
          PerfettoSqlTypes.STRING,
        ),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      // Create ModifyColumnsNode with IntervalIntersectNode as input
      const modifyNode = new ModifyColumnsNode({
        selectedColumns: [],
      });
      modifyNode.primaryInput = intervalNode;
      modifyNode.onPrevNodesUpdated();

      const selectedCols = modifyNode.state.selectedColumns;

      // Verify ts column has TIMESTAMP type
      const tsCol = selectedCols.find((c) => c.name === 'ts');
      expect(tsCol).toBeDefined();
      expect(tsCol?.type).toBe('TIMESTAMP');
      expect(tsCol?.column.type).toEqual(PerfettoSqlTypes.TIMESTAMP);

      // Verify dur column has DURATION type
      const durCol = selectedCols.find((c) => c.name === 'dur');
      expect(durCol).toBeDefined();
      expect(durCol?.type).toBe('DURATION');
      expect(durCol?.column.type).toEqual(PerfettoSqlTypes.DURATION);

      // Verify ts_0 column has TIMESTAMP type
      const ts0Col = selectedCols.find((c) => c.name === 'ts_0');
      expect(ts0Col).toBeDefined();
      expect(ts0Col?.type).toBe('TIMESTAMP');
      expect(ts0Col?.column.type).toEqual(PerfettoSqlTypes.TIMESTAMP);

      // Verify dur_0 column has DURATION type
      const dur0Col = selectedCols.find((c) => c.name === 'dur_0');
      expect(dur0Col).toBeDefined();
      expect(dur0Col?.type).toBe('DURATION');
      expect(dur0Col?.column.type).toEqual(PerfettoSqlTypes.DURATION);

      // Verify ts_1 column has TIMESTAMP type
      const ts1Col = selectedCols.find((c) => c.name === 'ts_1');
      expect(ts1Col).toBeDefined();
      expect(ts1Col?.type).toBe('TIMESTAMP');
      expect(ts1Col?.column.type).toEqual(PerfettoSqlTypes.TIMESTAMP);

      // Verify dur_1 column has DURATION type
      const dur1Col = selectedCols.find((c) => c.name === 'dur_1');
      expect(dur1Col).toBeDefined();
      expect(dur1Col?.type).toBe('DURATION');
      expect(dur1Col?.column.type).toEqual(PerfettoSqlTypes.DURATION);
    });

    it('should pass partition columns with original types to ModifyColumnsNode', () => {
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
        createColumnInfoWithSqlType('utid', 'INT', PerfettoSqlTypes.INT),
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
        createColumnInfoWithSqlType('utid', 'INT', PerfettoSqlTypes.INT),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        partitionColumns: ['utid'],
      });

      // Create ModifyColumnsNode with IntervalIntersectNode as input
      const modifyNode = new ModifyColumnsNode({
        selectedColumns: [],
      });
      modifyNode.primaryInput = intervalNode;
      modifyNode.onPrevNodesUpdated();

      const selectedCols = modifyNode.state.selectedColumns;

      // Verify utid column preserves its type
      const utidCol = selectedCols.find((c) => c.name === 'utid');
      expect(utidCol).toBeDefined();
      expect(utidCol?.type).toBe('INT');
      expect(utidCol?.column.type).toEqual(PerfettoSqlTypes.INT);
    });

    it('should pass all expected columns to ModifyColumnsNode', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('ts', 'INT64'),
        createColumnInfo('dur', 'INT64'),
        createColumnInfo('status', 'STRING'),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      // Create ModifyColumnsNode with IntervalIntersectNode as input
      const modifyNode = new ModifyColumnsNode({
        selectedColumns: [],
      });
      modifyNode.primaryInput = intervalNode;
      modifyNode.onPrevNodesUpdated();

      const colNames = modifyNode.state.selectedColumns.map((c) => c.name);

      // Should have all expected columns
      expect(colNames).toContain('ts');
      expect(colNames).toContain('dur');
      expect(colNames).toContain('id_0');
      expect(colNames).toContain('ts_0');
      expect(colNames).toContain('dur_0');
      expect(colNames).toContain('id_1');
      expect(colNames).toContain('ts_1');
      expect(colNames).toContain('dur_1');
      expect(colNames).toContain('name');
      expect(colNames).toContain('status');
    });

    it('should pass non-duplicated columns with original types to ModifyColumnsNode', () => {
      // Non-duplicated columns should preserve their types from input nodes
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
        createColumnInfoWithSqlType('name', 'STRING', PerfettoSqlTypes.STRING),
        createColumnInfoWithSqlType('value', 'DOUBLE', PerfettoSqlTypes.DOUBLE),
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
          'status',
          'STRING',
          PerfettoSqlTypes.STRING,
        ),
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      // Create ModifyColumnsNode with IntervalIntersectNode as input
      const modifyNode = new ModifyColumnsNode({
        selectedColumns: [],
      });
      modifyNode.primaryInput = intervalNode;
      modifyNode.onPrevNodesUpdated();

      const selectedCols = modifyNode.state.selectedColumns;

      // Verify non-duplicated columns preserve their types
      const nameCol = selectedCols.find((c) => c.name === 'name');
      expect(nameCol).toBeDefined();
      expect(nameCol?.type).toBe('STRING');
      expect(nameCol?.column.type).toEqual(PerfettoSqlTypes.STRING);

      const valueCol = selectedCols.find((c) => c.name === 'value');
      expect(valueCol).toBeDefined();
      expect(valueCol?.type).toBe('DOUBLE');
      expect(valueCol?.column.type).toEqual(PerfettoSqlTypes.DOUBLE);

      const statusCol = selectedCols.find((c) => c.name === 'status');
      expect(statusCol).toBeDefined();
      expect(statusCol?.type).toBe('STRING');
      expect(statusCol?.column.type).toEqual(PerfettoSqlTypes.STRING);
    });

    it('should have column.type set correctly on finalCols', () => {
      // Test that IntervalIntersectNode.finalCols has column.type set
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
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      const cols = intervalNode.finalCols;

      // Check ts column
      const tsCol = cols.find((c) => c.name === 'ts');
      expect(tsCol?.column.type).toEqual({kind: 'timestamp'});

      // Check dur column
      const durCol = cols.find((c) => c.name === 'dur');
      expect(durCol?.column.type).toEqual({kind: 'duration'});

      // Check ts_0 column
      const ts0Col = cols.find((c) => c.name === 'ts_0');
      expect(ts0Col?.column.type).toEqual({kind: 'timestamp'});

      // Check dur_0 column
      const dur0Col = cols.find((c) => c.name === 'dur_0');
      expect(dur0Col?.column.type).toEqual({kind: 'duration'});
    });

    it('should propagate partition columns to ModifyColumnsNode when added after creation', () => {
      // Create input nodes with partition columns available
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
        createColumnInfoWithSqlType('utid', 'INT', PerfettoSqlTypes.INT),
        createColumnInfoWithSqlType('track_id', 'INT', PerfettoSqlTypes.INT),
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
        createColumnInfoWithSqlType('utid', 'INT', PerfettoSqlTypes.INT),
        createColumnInfoWithSqlType('track_id', 'INT', PerfettoSqlTypes.INT),
      ]);

      // Create IntervalIntersectNode WITHOUT partition columns initially
      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
      });

      // Create ModifyColumnsNode with IntervalIntersectNode as input
      const modifyNode = new ModifyColumnsNode({
        selectedColumns: [],
      });
      modifyNode.primaryInput = intervalNode;
      intervalNode.nextNodes.push(modifyNode);
      modifyNode.onPrevNodesUpdated();

      // Verify initial columns (no partition columns yet)
      let selectedCols = modifyNode.state.selectedColumns;
      expect(selectedCols.find((c) => c.name === 'utid')).toBeUndefined();
      expect(selectedCols.find((c) => c.name === 'track_id')).toBeUndefined();

      // Count initial columns (should be: ts, dur, id_0, ts_0, dur_0, id_1, ts_1, dur_1)
      const initialColCount = selectedCols.length;
      expect(initialColCount).toBe(8);

      // NOW add partition columns to the interval intersect node
      intervalNode.state.partitionColumns = ['utid', 'track_id'];

      // Notify downstream nodes about the column change
      notifyNextNodes(intervalNode);
      intervalNode.state.onchange?.();

      // Verify that ModifyColumnsNode received the updated columns including partitions
      selectedCols = modifyNode.state.selectedColumns;

      // Should now include utid and track_id partition columns
      const utidCol = selectedCols.find((c) => c.name === 'utid');
      expect(utidCol).toBeDefined();
      expect(utidCol?.type).toBe('INT');
      expect(utidCol?.column.type).toEqual(PerfettoSqlTypes.INT);

      const trackIdCol = selectedCols.find((c) => c.name === 'track_id');
      expect(trackIdCol).toBeDefined();
      expect(trackIdCol?.type).toBe('INT');
      expect(trackIdCol?.column.type).toEqual(PerfettoSqlTypes.INT);

      // Verify the column count increased by 2 (the 2 partition columns)
      expect(selectedCols.length).toBe(initialColCount + 2);
    });

    it('should handle partition columns that do not exist in input nodes', () => {
      // Create input nodes WITHOUT the partition columns
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
      ]);

      // Create IntervalIntersectNode with partition columns that DON'T exist
      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        partitionColumns: ['utid', 'track_id'], // These columns don't exist!
      });

      const cols = intervalNode.finalCols;

      // Should still create partition columns, but with 'NA' type as fallback
      const utidCol = cols.find((c) => c.name === 'utid');
      expect(utidCol).toBeDefined();
      expect(utidCol?.type).toBe('NA');
      expect(utidCol?.column.type).toBeUndefined();

      const trackIdCol = cols.find((c) => c.name === 'track_id');
      expect(trackIdCol).toBeDefined();
      expect(trackIdCol?.type).toBe('NA');
      expect(trackIdCol?.column.type).toBeUndefined();
    });

    it('should use first input node type for partition columns when types differ', () => {
      // Create input nodes with DIFFERENT types for the same partition column
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
        createColumnInfoWithSqlType('utid', 'INT', PerfettoSqlTypes.INT),
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
        createColumnInfoWithSqlType('utid', 'STRING', PerfettoSqlTypes.STRING), // Different type!
      ]);

      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, node2],
        partitionColumns: ['utid'],
      });

      const cols = intervalNode.finalCols;

      // Should use the type from the FIRST input node
      const utidCol = cols.find((c) => c.name === 'utid');
      expect(utidCol).toBeDefined();
      expect(utidCol?.type).toBe('INT'); // From node1, not STRING from node2
      expect(utidCol?.column.type).toEqual(PerfettoSqlTypes.INT);
    });

    it('should handle empty input nodes gracefully', () => {
      // Edge case: What if an input node is undefined?
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
      ]);

      // Test edge case: undefined input node (implementation handles this gracefully)
      const intervalNode = new IntervalIntersectNode({
        inputNodes: [node1, undefined] as QueryNode[],
      });

      const cols = intervalNode.finalCols;

      // Should still have base columns
      expect(cols.find((c) => c.name === 'ts')).toBeDefined();
      expect(cols.find((c) => c.name === 'dur')).toBeDefined();
      expect(cols.find((c) => c.name === 'id_0')).toBeDefined();

      // Should NOT create columns for the undefined node
      expect(cols.find((c) => c.name === 'id_1')).toBeUndefined();
    });
  });
});
