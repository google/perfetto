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

import {MergeNode} from './merge_node';
import {QueryNode, NodeType} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';

describe('MergeNode', () => {
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
    it('should initialize with default values', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('value', 'INT'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      expect(mergeNode.state.leftQueryAlias).toBe('left');
      expect(mergeNode.state.rightQueryAlias).toBe('right');
      expect(mergeNode.state.conditionType).toBe('equality');
      expect(mergeNode.state.leftColumn).toBe('id');
      expect(mergeNode.state.rightColumn).toBe('id');
      expect(mergeNode.state.autoExecute).toBe(false);
    });

    it('should use default aliases when not provided', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: undefined!,
        rightQueryAlias: undefined!,
        conditionType: 'equality',
        leftColumn: '',
        rightColumn: '',
        sqlExpression: '',
      });

      expect(mergeNode.state.leftQueryAlias).toBe('left');
      expect(mergeNode.state.rightQueryAlias).toBe('right');
    });
  });

  describe('finalCols', () => {
    it('should return empty array when prevNodes length is not 2', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      expect(mergeNode.finalCols).toEqual([]);
    });

    it('should include equality column once without prefix', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('value', 'INT'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      const finalCols = mergeNode.finalCols;
      const idColumns = finalCols.filter((c) => c.name === 'id');

      expect(idColumns.length).toBe(1);
      expect(idColumns[0].type).toBe('INT');
    });

    it('should exclude duplicated columns', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('value', 'INT'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      const finalCols = mergeNode.finalCols;
      const colNames = finalCols.map((c) => c.name);

      // Should include 'id' once (equality column)
      expect(colNames.filter((n) => n === 'id').length).toBe(1);

      // Should NOT include 'name' (duplicated, not the equality column)
      expect(colNames).not.toContain('name');

      // Should include 'ts' (only in left)
      expect(colNames).toContain('ts');

      // Should include 'value' (only in right)
      expect(colNames).toContain('value');
    });

    it('should include all non-duplicated columns from both inputs', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('left_only_1', 'STRING'),
        createColumnInfo('left_only_2', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('right_only_1', 'STRING'),
        createColumnInfo('right_only_2', 'INT64'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      const finalCols = mergeNode.finalCols;
      const colNames = finalCols.map((c) => c.name);

      expect(colNames).toContain('id');
      expect(colNames).toContain('left_only_1');
      expect(colNames).toContain('left_only_2');
      expect(colNames).toContain('right_only_1');
      expect(colNames).toContain('right_only_2');
      expect(colNames.length).toBe(5);
    });

    it('should handle equality on different column names', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('parent_id', 'INT'),
        createColumnInfo('value', 'INT'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'parent_id',
        sqlExpression: '',
      });

      const finalCols = mergeNode.finalCols;
      const colNames = finalCols.map((c) => c.name);

      // When joining on different column names (id = parent_id),
      // both should be included if they're not duplicated
      expect(colNames).toContain('id');
      expect(colNames).toContain('parent_id');
      expect(colNames).toContain('name');
      expect(colNames).toContain('ts');
      expect(colNames).toContain('value');
      expect(colNames.length).toBe(5);
    });

    it('should handle equality on same column name with all duplicates', () => {
      // This is the slice-to-slice scenario: joining slice with slice on id = id
      // When both tables have identical columns and we join on id = id,
      // we should only get 'id' once, and all other duplicates should be excluded
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      const finalCols = mergeNode.finalCols;
      const colNames = finalCols.map((c) => c.name);

      // Should only include 'id' once (the equality column)
      expect(colNames).toContain('id');
      expect(colNames.filter((n) => n === 'id').length).toBe(1);

      // Should NOT include 'name' or 'ts' (duplicated across both inputs)
      expect(colNames).not.toContain('name');
      expect(colNames).not.toContain('ts');

      // Final result should only have the 'id' column
      expect(colNames.length).toBe(1);
    });

    it('should not include equality columns in freeform mode', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('parent_id', 'INT'),
        createColumnInfo('value', 'INT'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 't1',
        rightQueryAlias: 't2',
        conditionType: 'freeform',
        leftColumn: '',
        rightColumn: '',
        sqlExpression: 't1.id = t2.parent_id',
      });

      const finalCols = mergeNode.finalCols;
      const colNames = finalCols.map((c) => c.name);

      // In freeform mode, no columns are treated as equality columns
      // id is duplicated, so it should be excluded
      expect(colNames).not.toContain('id');

      // Non-duplicated columns should be included
      expect(colNames).toContain('name');
      expect(colNames).toContain('ts');
      expect(colNames).toContain('parent_id');
      expect(colNames).toContain('value');
    });

    it('should handle empty column lists', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: '',
        rightColumn: '',
        sqlExpression: '',
      });

      expect(mergeNode.finalCols).toEqual([]);
    });

    it('should set all columns as checked', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('value', 'INT'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      const finalCols = mergeNode.finalCols;

      expect(finalCols.every((c) => c.checked === true)).toBe(true);
    });
  });

  describe('validation', () => {
    it('should fail when prevNodes length is not 2', () => {
      const node1 = createMockPrevNode('node1', []);

      const mergeNode = new MergeNode({
        prevNodes: [node1],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      expect(mergeNode.validate()).toBe(false);
      expect(mergeNode.state.issues?.queryError?.message).toContain(
        'exactly two sources',
      );
    });

    it('should pass when aliases are set by constructor defaults', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: undefined!,
        rightQueryAlias: undefined!,
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      // Constructor sets default aliases to 'left' and 'right', so this should pass
      expect(mergeNode.validate()).toBe(true);
    });

    it('should fail when equality columns are missing in equality mode', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: '',
        rightColumn: '',
        sqlExpression: '',
      });

      expect(mergeNode.validate()).toBe(false);
      expect(mergeNode.state.issues?.queryError?.message).toContain(
        'Both left and right columns are required',
      );
    });

    it('should fail when SQL expression is missing in freeform mode', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'freeform',
        leftColumn: '',
        rightColumn: '',
        sqlExpression: '',
      });

      expect(mergeNode.validate()).toBe(false);
      expect(mergeNode.state.issues?.queryError?.message).toContain(
        'SQL expression',
      );
    });

    it('should pass validation with valid equality condition', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      expect(mergeNode.validate()).toBe(true);
    });

    it('should fail when all columns are duplicated', () => {
      // Freeform mode with all columns duplicated - no equality column special handling
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'freeform',
        leftColumn: '',
        rightColumn: '',
        sqlExpression: 'left.id = right.id',
      });

      expect(mergeNode.validate()).toBe(false);
      expect(mergeNode.state.issues?.queryError?.message).toContain(
        'No columns to expose',
      );
      expect(mergeNode.state.issues?.queryError?.message).toContain(
        'Modify Columns',
      );
    });

    it('should pass validation with valid freeform condition', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('parent_id', 'INT'),
      ]);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 't1',
        rightQueryAlias: 't2',
        conditionType: 'freeform',
        leftColumn: '',
        rightColumn: '',
        sqlExpression: 't1.id = t2.parent_id',
      });

      expect(mergeNode.validate()).toBe(true);
    });
  });

  describe('getTitle', () => {
    it('should return "Merge"', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: '',
        rightColumn: '',
        sqlExpression: '',
      });

      expect(mergeNode.getTitle()).toBe('Merge');
    });
  });

  describe('getInputLabels', () => {
    it('should return the left and right query aliases', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'foo',
        rightQueryAlias: 'bar',
        conditionType: 'equality',
        leftColumn: '',
        rightColumn: '',
        sqlExpression: '',
      });

      expect(mergeNode.getInputLabels()).toEqual(['foo', 'bar']);
    });
  });

  describe('clone', () => {
    it('should create a deep copy of the node', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      const cloned = mergeNode.clone() as MergeNode;

      expect(cloned).not.toBe(mergeNode);
      expect(cloned.state.leftQueryAlias).toBe('left');
      expect(cloned.state.rightQueryAlias).toBe('right');
      expect(cloned.state.conditionType).toBe('equality');
      expect(cloned.state.leftColumn).toBe('id');
      expect(cloned.state.rightColumn).toBe('id');
    });

    it('should not share state with original', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      const cloned = mergeNode.clone() as MergeNode;

      // Modify the cloned state
      cloned.state.leftQueryAlias = 'modified';

      // Original should not be affected
      expect(mergeNode.state.leftQueryAlias).toBe('left');
    });
  });

  describe('getStructuredQuery', () => {
    it('should return undefined if validation fails', () => {
      const node1 = createMockPrevNode('node1', []);

      const mergeNode = new MergeNode({
        prevNodes: [node1],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: '',
        rightColumn: '',
        sqlExpression: '',
      });

      expect(mergeNode.getStructuredQuery()).toBeUndefined();
    });

    it('should include select_columns in the structured query', () => {
      // Create mock nodes with proper getStructuredQuery implementation
      const mockSq1 = new protos.PerfettoSqlStructuredQuery();
      const mockSq2 = new protos.PerfettoSqlStructuredQuery();

      const node1 = {
        ...createMockPrevNode('node1', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('name', 'STRING'),
        ]),
        getStructuredQuery: () => mockSq1,
      } as QueryNode;

      const node2 = {
        ...createMockPrevNode('node2', [
          createColumnInfo('id', 'INT'),
          createColumnInfo('value', 'INT'),
        ]),
        getStructuredQuery: () => mockSq2,
      } as QueryNode;

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      const sq = mergeNode.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.selectColumns).toBeDefined();

      const selectColNames = sq?.selectColumns?.map(
        (c) => c.columnNameOrExpression,
      );
      const finalColNames = mergeNode.finalCols.map((c) => c.name);

      expect(selectColNames).toEqual(finalColNames);
    });

    it('should create equality join condition for equality mode', () => {
      // Create mock nodes with proper getStructuredQuery implementation
      const mockSq1 = new protos.PerfettoSqlStructuredQuery();
      const mockSq2 = new protos.PerfettoSqlStructuredQuery();

      const node1 = {
        ...createMockPrevNode('node1', [createColumnInfo('id', 'INT')]),
        getStructuredQuery: () => mockSq1,
      } as QueryNode;

      const node2 = {
        ...createMockPrevNode('node2', [createColumnInfo('id', 'INT')]),
        getStructuredQuery: () => mockSq2,
      } as QueryNode;

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      const sq = mergeNode.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.experimentalJoin).toBeDefined();
      expect(sq?.experimentalJoin?.equalityColumns).toBeDefined();
      expect(sq?.experimentalJoin?.equalityColumns?.leftColumn).toBe('id');
      expect(sq?.experimentalJoin?.equalityColumns?.rightColumn).toBe('id');
    });

    it('should create freeform join condition for freeform mode', () => {
      // Create mock nodes with proper getStructuredQuery implementation
      const mockSq1 = new protos.PerfettoSqlStructuredQuery();
      const mockSq2 = new protos.PerfettoSqlStructuredQuery();

      const node1 = {
        ...createMockPrevNode('node1', [createColumnInfo('id', 'INT')]),
        getStructuredQuery: () => mockSq1,
      } as QueryNode;

      const node2 = {
        ...createMockPrevNode('node2', [createColumnInfo('parent_id', 'INT')]),
        getStructuredQuery: () => mockSq2,
      } as QueryNode;

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 't1',
        rightQueryAlias: 't2',
        conditionType: 'freeform',
        leftColumn: '',
        rightColumn: '',
        sqlExpression: 't1.id = t2.parent_id',
      });

      const sq = mergeNode.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.experimentalJoin).toBeDefined();
      expect(sq?.experimentalJoin?.freeformCondition).toBeDefined();
      expect(sq?.experimentalJoin?.freeformCondition?.leftQueryAlias).toBe(
        't1',
      );
      expect(sq?.experimentalJoin?.freeformCondition?.rightQueryAlias).toBe(
        't2',
      );
      expect(sq?.experimentalJoin?.freeformCondition?.sqlExpression).toBe(
        't1.id = t2.parent_id',
      );
    });
  });

  describe('serializeState', () => {
    it('should serialize all state fields', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const mergeNode = new MergeNode({
        prevNodes: [node1, node2],
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
        comment: 'Test merge',
      });

      const serialized = mergeNode.serializeState();

      expect(serialized.leftNodeId).toBe('node1');
      expect(serialized.rightNodeId).toBe('node2');
      expect(serialized.leftQueryAlias).toBe('left');
      expect(serialized.rightQueryAlias).toBe('right');
      expect(serialized.conditionType).toBe('equality');
      expect(serialized.leftColumn).toBe('id');
      expect(serialized.rightColumn).toBe('id');
      expect(serialized.comment).toBe('Test merge');
    });
  });

  describe('deserializeState', () => {
    it('should deserialize state correctly', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);
      const nodes = new Map([
        ['node1', node1],
        ['node2', node2],
      ]);

      const state = MergeNode.deserializeState(nodes, {
        leftNodeId: 'node1',
        rightNodeId: 'node2',
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      expect(state.prevNodes.length).toBe(2);
      expect(state.prevNodes[0]).toBe(node1);
      expect(state.prevNodes[1]).toBe(node2);
      expect(state.leftQueryAlias).toBe('left');
      expect(state.rightQueryAlias).toBe('right');
      expect(state.conditionType).toBe('equality');
      expect(state.leftColumn).toBe('id');
      expect(state.rightColumn).toBe('id');
    });

    it('should handle missing nodes gracefully', () => {
      const nodes = new Map<string, QueryNode>();

      const state = MergeNode.deserializeState(nodes, {
        leftNodeId: 'missing1',
        rightNodeId: 'missing2',
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
      });

      expect(state.prevNodes.length).toBe(0);
    });
  });
});
