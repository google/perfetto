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

import {AddColumnsNode, AddColumnsNodeState} from './add_columns_node';
import {QueryNode} from '../../query_node';
import protos from '../../../../protos';
import {createMockNode, createColumnInfo} from '../testing/test_utils';

describe('AddColumnsNode', () => {
  function createMockPrimaryNode(): QueryNode {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = 'primary';
    const table = new protos.PerfettoSqlStructuredQuery.Table();
    table.tableName = 'primary_table';
    table.columnNames = ['id', 'ts', 'dur'];
    sq.table = table;

    return createMockNode({
      nodeId: 'primary',
      columns: [
        createColumnInfo('id', 'int'),
        createColumnInfo('ts', 'int'),
        createColumnInfo('dur', 'int'),
      ],
      getTitle: () => 'Primary Table',
      getStructuredQuery: () => sq,
    });
  }

  function createMockSecondaryNode(): QueryNode {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = 'secondary';
    const table = new protos.PerfettoSqlStructuredQuery.Table();
    table.tableName = 'secondary_table';
    table.columnNames = ['id', 'name', 'category'];
    sq.table = table;

    return createMockNode({
      nodeId: 'secondary',
      columns: [
        createColumnInfo('id', 'int'),
        createColumnInfo('name', 'string'),
        createColumnInfo('category', 'string'),
      ],
      getTitle: () => 'Secondary Table',
      getStructuredQuery: () => sq,
    });
  }

  function createAddColumnsNodeWithInputs(
    state: AddColumnsNodeState,
    primaryNode?: QueryNode,
    secondaryNode?: QueryNode,
  ): AddColumnsNode {
    const node = new AddColumnsNode(state);
    if (primaryNode) {
      primaryNode.nextNodes.push(node);
      node.primaryInput = primaryNode;
    }
    if (secondaryNode) {
      // Set the secondary input connection at port 0
      secondaryNode.nextNodes.push(node);
      node.secondaryInputs.connections.set(0, secondaryNode);
    }
    return node;
  }

  describe('getStructuredQuery', () => {
    it('should generate query with only computed columns (no JOIN)', () => {
      const primaryNode = createMockPrimaryNode();
      const node = createAddColumnsNodeWithInputs(
        {
          computedColumns: [
            {
              expression: 'dur / 1e6',
              name: 'dur_ms',
            },
            {
              expression: 'ts + dur',
              name: 'end_ts',
            },
          ],
        },
        primaryNode,
      );

      const query = node.getStructuredQuery();

      // Should successfully generate a query
      expect(query).toBeDefined();
      expect(query?.id).toBe(node.nodeId);

      // Should have select columns with the computed expressions
      expect(query?.selectColumns).toBeDefined();
      const selectCols = query?.selectColumns ?? [];
      expect(selectCols.length).toBeGreaterThan(0);

      // Check that computed columns are included by looking for their aliases
      const aliases = selectCols
        .map((col) => col.alias)
        .filter((a): a is string => a !== undefined);
      expect(aliases).toContain('dur_ms');
      expect(aliases).toContain('end_ts');
    });

    it('should pass through query when no columns are added', () => {
      const primaryNode = createMockPrimaryNode();
      const node = createAddColumnsNodeWithInputs(
        {
          computedColumns: [],
        },
        primaryNode,
      );

      const query = node.getStructuredQuery();

      // Should return the primary input's query
      expect(query).toEqual(primaryNode.getStructuredQuery());
    });

    it('should skip invalid computed columns', () => {
      const primaryNode = createMockPrimaryNode();
      const node = createAddColumnsNodeWithInputs(
        {
          computedColumns: [
            {
              expression: 'dur / 1e6',
              name: 'dur_ms',
            },
            {
              expression: '', // Invalid: empty expression
              name: 'invalid',
            },
            {
              expression: 'ts + dur',
              name: '', // Invalid: empty name
            },
          ],
        },
        primaryNode,
      );

      const query = node.getStructuredQuery();

      // Should successfully generate a query (skipping invalid columns)
      expect(query).toBeDefined();

      // Should only include the valid computed column
      const selectCols = query?.selectColumns ?? [];
      const aliases = selectCols
        .map((col) => col.alias)
        .filter((a): a is string => !!a && a.trim() !== '');

      // Should have the valid column 'dur_ms'
      expect(aliases).toContain('dur_ms');

      // Should NOT have the invalid columns
      expect(aliases).not.toContain('invalid');
    });

    it('should generate query with both JOIN columns and computed columns', () => {
      const primaryNode = createMockPrimaryNode();
      const secondaryNode = createMockSecondaryNode();
      const node = createAddColumnsNodeWithInputs(
        {
          selectedColumns: ['name', 'category'],
          leftColumn: 'id',
          rightColumn: 'id',
          computedColumns: [
            {
              expression: 'dur / 1e6',
              name: 'dur_ms',
            },
            {
              expression: 'UPPER(name)',
              name: 'name_upper',
            },
          ],
        },
        primaryNode,
        secondaryNode,
      );

      const query = node.getStructuredQuery();

      // The key test: should successfully generate a query that includes both JOIN and computed columns
      expect(query).toBeDefined();
      expect(query?.id).toBe(node.nodeId);

      // Should have select columns (computed columns are added via SELECT)
      expect(query?.selectColumns).toBeDefined();
      const selectCols = query?.selectColumns ?? [];
      expect(selectCols.length).toBeGreaterThan(0);

      // Verify that BOTH JOIN columns AND computed columns are present
      const aliases = selectCols
        .map((col) => col.alias)
        .filter((a): a is string => a !== undefined);

      // JOIN columns from secondary table
      expect(aliases).toContain('name');
      expect(aliases).toContain('category');

      // Computed columns
      expect(aliases).toContain('dur_ms');
      expect(aliases).toContain('name_upper');

      // This verifies the bug fix: previously computed columns would be silently ignored after a JOIN
    });

    it('should handle computed columns when JOIN columns list is empty', () => {
      const primaryNode = createMockPrimaryNode();
      const secondaryNode = createMockSecondaryNode();
      const node = createAddColumnsNodeWithInputs(
        {
          selectedColumns: [], // No JOIN columns
          leftColumn: 'id',
          rightColumn: 'id',
          computedColumns: [
            {
              expression: 'dur / 1e6',
              name: 'dur_ms',
            },
          ],
        },
        primaryNode,
        secondaryNode,
      );

      const query = node.getStructuredQuery();

      // Should successfully generate a query with just computed columns
      expect(query).toBeDefined();
      expect(query?.selectColumns).toBeDefined();
    });

    it('should handle computed columns with referenced modules', () => {
      const primaryNode = createMockPrimaryNode();
      const node = createAddColumnsNodeWithInputs(
        {
          computedColumns: [
            {
              expression: 'android.some_function(dur)',
              name: 'custom_dur',
              module: 'android',
            },
            {
              expression: 'dur / 1e6',
              name: 'dur_ms',
              // No module
            },
          ],
        },
        primaryNode,
      );

      const query = node.getStructuredQuery();

      // Should successfully generate a query with module references
      expect(query).toBeDefined();
      expect(query?.referencedModules).toBeDefined();
      expect(query?.referencedModules).toContain('android');
    });
  });

  describe('isApplyDisabled', () => {
    it('should disable Apply when rightNode exists but leftColumn is not set', () => {
      const primaryNode = createMockPrimaryNode();
      const secondaryNode = createMockSecondaryNode();
      const node = createAddColumnsNodeWithInputs(
        {
          selectedColumns: ['name'],
          leftColumn: '', // Empty string (user cleared the field)
          rightColumn: 'id',
        },
        primaryNode,
        secondaryNode,
      );

      const isDisabled = node.isApplyDisabled();

      expect(isDisabled).toBe(true);
    });

    it('should disable Apply when rightNode exists but rightColumn is not set', () => {
      const primaryNode = createMockPrimaryNode();
      const secondaryNode = createMockSecondaryNode();
      const node = createAddColumnsNodeWithInputs(
        {
          selectedColumns: ['name'],
          leftColumn: 'id',
          rightColumn: '', // Empty string (user cleared the field)
        },
        primaryNode,
        secondaryNode,
      );

      const isDisabled = node.isApplyDisabled();

      expect(isDisabled).toBe(true);
    });

    it('should disable Apply when rightNode exists but both join columns are not set', () => {
      const primaryNode = createMockPrimaryNode();
      const secondaryNode = createMockSecondaryNode();
      const node = createAddColumnsNodeWithInputs(
        {
          selectedColumns: ['name'],
          leftColumn: '', // Empty string (user cleared the field)
          rightColumn: '', // Empty string (user cleared the field)
        },
        primaryNode,
        secondaryNode,
      );

      const isDisabled = node.isApplyDisabled();

      expect(isDisabled).toBe(true);
    });

    it('should enable Apply when rightNode exists with both join columns and selected columns', () => {
      const primaryNode = createMockPrimaryNode();
      const secondaryNode = createMockSecondaryNode();
      const node = createAddColumnsNodeWithInputs(
        {
          selectedColumns: ['name', 'category'],
          leftColumn: 'id',
          rightColumn: 'id',
        },
        primaryNode,
        secondaryNode,
      );

      const isDisabled = node.isApplyDisabled();

      expect(isDisabled).toBe(false);
    });

    it('should disable Apply when rightNode exists with join columns but no selected columns', () => {
      const primaryNode = createMockPrimaryNode();
      const secondaryNode = createMockSecondaryNode();
      const node = createAddColumnsNodeWithInputs(
        {
          selectedColumns: [], // Empty
          leftColumn: 'id',
          rightColumn: 'id',
        },
        primaryNode,
        secondaryNode,
      );

      const isDisabled = node.isApplyDisabled();

      expect(isDisabled).toBe(true);
    });
  });
});
