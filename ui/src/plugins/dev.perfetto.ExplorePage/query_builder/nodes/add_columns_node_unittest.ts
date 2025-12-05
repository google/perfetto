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
import {QueryNode, NodeType} from '../../query_node';
import protos from '../../../../protos';

describe('AddColumnsNode', () => {
  function createMockPrimaryNode(): QueryNode {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = 'primary';
    const table = new protos.PerfettoSqlStructuredQuery.Table();
    table.tableName = 'primary_table';
    sq.table = table;

    return {
      nodeId: 'primary',
      type: NodeType.kTable,
      nextNodes: [],
      finalCols: [
        {
          name: 'id',
          type: 'INT',
          checked: true,
          column: {name: 'id'},
        },
        {
          name: 'ts',
          type: 'INT',
          checked: true,
          column: {name: 'ts'},
        },
        {
          name: 'dur',
          type: 'INT',
          checked: true,
          column: {name: 'dur'},
        },
      ],
      state: {},
      validate: () => true,
      getTitle: () => 'Primary Table',
      nodeSpecificModify: () => null,
      nodeDetails: () => ({content: null}),
      nodeInfo: () => null,
      clone: () => createMockPrimaryNode(),
      getStructuredQuery: () => sq,
      serializeState: () => ({}),
    } as QueryNode;
  }

  function createMockSecondaryNode(): QueryNode {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = 'secondary';
    const table = new protos.PerfettoSqlStructuredQuery.Table();
    table.tableName = 'secondary_table';
    sq.table = table;

    return {
      nodeId: 'secondary',
      type: NodeType.kTable,
      nextNodes: [],
      finalCols: [
        {
          name: 'id',
          type: 'INT',
          checked: true,
          column: {name: 'id'},
        },
        {
          name: 'name',
          type: 'STRING',
          checked: true,
          column: {name: 'name'},
        },
        {
          name: 'category',
          type: 'STRING',
          checked: true,
          column: {name: 'category'},
        },
      ],
      state: {},
      validate: () => true,
      getTitle: () => 'Secondary Table',
      nodeSpecificModify: () => null,
      nodeDetails: () => ({content: null}),
      nodeInfo: () => null,
      clone: () => createMockSecondaryNode(),
      getStructuredQuery: () => sq,
      serializeState: () => ({}),
    } as QueryNode;
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
        .filter((a): a is string => a !== undefined);

      // Should have the valid column 'dur_ms'
      expect(aliases).toContain('dur_ms');

      // Should NOT have the invalid columns
      expect(aliases).not.toContain('invalid');
      expect(aliases).not.toContain('');
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
});
