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
import {QueryNode, NodeType} from '../../query_node';
import {ColumnInfo} from '../column_info';

describe('IntervalIntersectNode', () => {
  function createMockPrevNode(id: string, columns: ColumnInfo[]): QueryNode {
    return {
      nodeId: id,
      type: NodeType.kTable,
      nextNodes: [],
      finalCols: columns,
      state: {},
      validate: () => true,
      getTitle: () => `Mock ${id}`,
      nodeSpecificModify: () => null,
      nodeInfo: () => null,
      clone: () => createMockPrevNode(id, columns),
      getStructuredQuery: () => undefined,
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2, node3],
        filterNegativeDur: [false],
      });

      expect(node.state.filterNegativeDur).toEqual([false, true, true]);
    });

    it('should set autoExecute to false by default', () => {
      const node = new IntervalIntersectNode({
        prevNodes: [],
      });

      expect(node.state.autoExecute).toBe(false);
    });
  });

  describe('finalCols', () => {
    it('should return empty array when no prev nodes', () => {
      const node = new IntervalIntersectNode({
        prevNodes: [],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2, node3],
      });

      const cols = node.finalCols;

      // After ts, dur (indexes 0, 1), we should have id_0, ts_0, dur_0
      expect(cols[2].name).toBe('id_0');
      expect(cols[2].type).toBe('INT');
      expect(cols[3].name).toBe('ts_0');
      expect(cols[3].type).toBe('INT64');
      expect(cols[4].name).toBe('dur_0');
      expect(cols[4].type).toBe('INT64');

      // Then id_1, ts_1, dur_1
      expect(cols[5].name).toBe('id_1');
      expect(cols[5].type).toBe('INT');
      expect(cols[6].name).toBe('ts_1');
      expect(cols[6].type).toBe('INT64');
      expect(cols[7].name).toBe('dur_1');
      expect(cols[7].type).toBe('INT64');

      // Then id_2, ts_2, dur_2
      expect(cols[8].name).toBe('id_2');
      expect(cols[8].type).toBe('INT');
      expect(cols[9].name).toBe('ts_2');
      expect(cols[9].type).toBe('INT64');
      expect(cols[10].name).toBe('dur_2');
      expect(cols[10].type).toBe('INT64');
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2, node3],
      });

      const cols = node.finalCols;

      // Check id_N, ts_N, dur_N types for each input
      const id0 = cols.find((c) => c.name === 'id_0');
      expect(id0?.type).toBe('INT');
      const ts0 = cols.find((c) => c.name === 'ts_0');
      expect(ts0?.type).toBe('INT64');
      const dur0 = cols.find((c) => c.name === 'dur_0');
      expect(dur0?.type).toBe('INT64');

      const id1 = cols.find((c) => c.name === 'id_1');
      expect(id1?.type).toBe('INT');
      const ts1 = cols.find((c) => c.name === 'ts_1');
      expect(ts1?.type).toBe('INT64');
      const dur1 = cols.find((c) => c.name === 'dur_1');
      expect(dur1?.type).toBe('INT64');

      const id2 = cols.find((c) => c.name === 'id_2');
      expect(id2?.type).toBe('INT');
      const ts2 = cols.find((c) => c.name === 'ts_2');
      expect(ts2?.type).toBe('INT64');
      const dur2 = cols.find((c) => c.name === 'dur_2');
      expect(dur2?.type).toBe('INT64');

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
        prevNodes: [node1, node2],
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
        prevNodes: [node1],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1],
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
      node.prevNodes.push(node2);

      // Second validation should pass and clear errors
      expect(node.validate()).toBe(true);
      expect(node.state.issues?.queryError).toBeUndefined();
    });
  });

  describe('getTitle', () => {
    it('should return "Interval Intersect"', () => {
      const node = new IntervalIntersectNode({
        prevNodes: [],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2],
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
        prevNodes: [node1, node2, node3],
        filterNegativeDur: [true, false, true],
        partitionColumns: ['utid'],
      });

      const serialized = node.serializeState();

      expect(serialized.intervalNodes).toEqual(['node2', 'node3']);
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
        prevNodes: [node1, node2],
      });

      const serialized = node.serializeState();

      expect(serialized.intervalNodes).toEqual(['node2']);
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
        intervalNodes: ['node2', 'node3'],
        filterNegativeDur: [true, false, true],
        partitionColumns: ['utid'],
      };

      const deserialized = IntervalIntersectNode.deserializeState(
        nodes,
        serialized,
        node1,
      );

      expect(deserialized.prevNodes).toEqual([node1, node2, node3]);
      expect(deserialized.filterNegativeDur).toEqual([true, false, true]);
      expect(deserialized.partitionColumns).toEqual(['utid']);
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
        intervalNodes: ['node2', 'node_missing'],
        filterNegativeDur: [true, false],
        partitionColumns: ['utid'],
      };

      const deserialized = IntervalIntersectNode.deserializeState(
        nodes,
        serialized,
        node1,
      );

      // Should only include found nodes
      expect(deserialized.prevNodes).toEqual([node1, node2]);
      expect(deserialized.filterNegativeDur).toEqual([true, false]);
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
        prevNodes: [node1, node2],
      });

      // Clear filterNegativeDur
      node.state.filterNegativeDur = undefined;

      node.onPrevNodesUpdated();

      expect(node.state.filterNegativeDur).toBeDefined();
      expect(node.state.filterNegativeDur).toEqual([true, true]);
    });

    it('should compact filterNegativeDur when prevNodes shrinks', () => {
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
        prevNodes: [node1, node2, node3],
        filterNegativeDur: [true, false, true],
      });

      // Remove one prev node
      node.prevNodes.pop();

      node.onPrevNodesUpdated();

      expect(node.state.filterNegativeDur).toEqual([true, false]);
    });

    it('should expand filterNegativeDur when prevNodes grows', () => {
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
        prevNodes: [node1, node2],
        filterNegativeDur: [true, false],
      });

      // Add another prev node
      node.prevNodes.push(node3);

      node.onPrevNodesUpdated();

      expect(node.state.filterNegativeDur).toEqual([true, false, true]);
    });
  });
});
