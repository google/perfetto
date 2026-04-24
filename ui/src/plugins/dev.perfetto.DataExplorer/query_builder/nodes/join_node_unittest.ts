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

import {JoinNode, JoinNodeAttrs} from './join_node';
import {QueryNode, NodeType} from '../../query_node';
import {ColumnInfo} from '../column_info';
import {PerfettoSqlTypes} from '../../../../trace_processor/perfetto_sql_type';
import protos from '../../../../protos';

describe('JoinNode', () => {
  function createMockPrevNode(id: string, columns: ColumnInfo[]): QueryNode {
    return {
      nodeId: id,
      type: NodeType.kTable,
      nextNodes: [],
      finalCols: columns,
      attrs: {},
      context: {},
      validate: () => true,
      getTitle: () => `Mock ${id}`,
      nodeSpecificModify: () => null,
      nodeDetails: () => ({content: null}),
      nodeInfo: () => null,
      clone: () => createMockPrevNode(id, columns),
      getStructuredQuery: () => undefined,
    } as QueryNode;
  }

  function createColumnInfo(
    name: string,
    _type: string,
    checked: boolean = true,
  ): ColumnInfo {
    return {
      name,
      checked,
    };
  }

  // Helper to create pre-checked column arrays for join node state
  function createCheckedColumns(
    columns: Array<{name: string; type?: string; checked?: boolean}>,
  ): ColumnInfo[] {
    return columns.map((c) => ({
      name: c.name,
      checked: c.checked ?? true,
    }));
  }

  // Helper to create a JoinNode and connect left/right secondary inputs
  function createJoinNodeWithInputs(
    leftNode: QueryNode | undefined,
    rightNode: QueryNode | undefined,
    attrs: Partial<{
      leftQueryAlias: string;
      rightQueryAlias: string;
      conditionType: 'equality' | 'freeform';
      joinType: 'INNER' | 'LEFT';
      leftColumn: string;
      rightColumn: string;
      sqlExpression: string;
      leftColumns: ColumnInfo[] | undefined;
      rightColumns: ColumnInfo[] | undefined;
    }>,
    context: Record<string, unknown> = {},
  ): JoinNode {
    const node = new JoinNode(
      {
        leftQueryAlias: attrs.leftQueryAlias ?? 'left',
        rightQueryAlias: attrs.rightQueryAlias ?? 'right',
        conditionType: attrs.conditionType ?? 'equality',
        joinType: attrs.joinType ?? 'INNER',
        leftColumn: attrs.leftColumn ?? '',
        rightColumn: attrs.rightColumn ?? '',
        sqlExpression: attrs.sqlExpression ?? '',
        leftColumns: attrs.leftColumns,
        rightColumns: attrs.rightColumns,
      },
      context,
    );
    if (leftNode) {
      node.secondaryInputs.connections.set(0, leftNode);
      leftNode.nextNodes.push(node);
    }
    if (rightNode) {
      node.secondaryInputs.connections.set(1, rightNode);
      rightNode.nextNodes.push(node);
    }
    if (leftNode || rightNode) {
      node.onPrevNodesUpdated?.();
    }
    return node;
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

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      expect(joinNode.attrs.leftQueryAlias).toBe('left');
      expect(joinNode.attrs.rightQueryAlias).toBe('right');
      expect(joinNode.attrs.conditionType).toBe('equality');
      expect(joinNode.attrs.leftColumn).toBe('id');
      expect(joinNode.attrs.rightColumn).toBe('id');
      expect(joinNode.context.autoExecute).toBe(false);
    });

    it('should use default aliases when not provided', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: undefined!,
          rightQueryAlias: undefined!,
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: '',
          rightColumn: '',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      expect(joinNode.attrs.leftQueryAlias).toBe('left');
      expect(joinNode.attrs.rightQueryAlias).toBe('right');
    });
  });

  describe('finalCols', () => {
    it('should return empty array when only one node is provided', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);

      const joinNode = createJoinNodeWithInputs(
        node1,
        undefined,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      expect(joinNode.finalCols).toEqual([]);
    });

    it('should return empty when no columns are checked', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('value', 'INT'),
      ]);

      // When leftColumns/rightColumns are undefined, updateColumnArrays()
      // initializes all columns with checked: false
      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      // All columns default to unchecked, so finalCols should be empty
      expect(joinNode.finalCols).toEqual([]);
    });

    it('should return only checked columns from left source', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('value', 'INT'),
      ]);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'value',
          sqlExpression: '', // Pre-set leftColumns with only 'id' checked
          leftColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: true},
            {name: 'name', type: 'STRING', checked: false},
          ]),
          rightColumns: createCheckedColumns([
            {name: 'value', type: 'INT', checked: false},
          ]),
        },
        {},
      );

      const finalCols = joinNode.finalCols;
      expect(finalCols.length).toBe(1);
      expect(finalCols[0].name).toBe('id');
    });

    it('should return only checked columns from right source', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('parent_id', 'INT'),
        createColumnInfo('value', 'INT'),
      ]);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'parent_id',
          sqlExpression: '',
          leftColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: false},
          ]),
          rightColumns: createCheckedColumns([
            {name: 'parent_id', type: 'INT', checked: true},
            {name: 'value', type: 'INT', checked: true},
          ]),
        },
        {},
      );

      const finalCols = joinNode.finalCols;
      const colNames = finalCols.map((c: ColumnInfo) => c.name);

      expect(colNames).toContain('parent_id');
      expect(colNames).toContain('value');
      expect(colNames.length).toBe(2);
    });

    it('should return checked columns from both sources', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
        createColumnInfo('ts', 'INT64'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('parent_id', 'INT'),
        createColumnInfo('value', 'INT'),
      ]);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'parent_id',
          sqlExpression: '',
          leftColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: true},
            {name: 'name', type: 'STRING', checked: false},
            {name: 'ts', type: 'INT64', checked: true},
          ]),
          rightColumns: createCheckedColumns([
            {name: 'parent_id', type: 'INT', checked: false},
            {name: 'value', type: 'INT', checked: true},
          ]),
        },
        {},
      );

      const finalCols = joinNode.finalCols;
      const colNames = finalCols.map((c: ColumnInfo) => c.name);

      expect(colNames).toContain('id');
      expect(colNames).toContain('ts');
      expect(colNames).toContain('value');
      expect(colNames).not.toContain('name');
      expect(colNames).not.toContain('parent_id');
      expect(colNames.length).toBe(3);
    });

    it('should handle empty column lists', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: '',
          rightColumn: '',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      expect(joinNode.finalCols).toEqual([]);
    });

    it('should return columns with checked=true status', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('value', 'INT'),
      ]);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'value',
          sqlExpression: '',
          leftColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: true},
            {name: 'name', type: 'STRING', checked: true},
          ]),
          rightColumns: createCheckedColumns([
            {name: 'value', type: 'INT', checked: true},
          ]),
        },
        {},
      );

      const finalCols = joinNode.finalCols;

      expect(finalCols.every((c) => c.checked === true)).toBe(true);
    });

    it('should preserve column aliases in finalCols', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
      ]);

      const leftCols = createCheckedColumns([
        {name: 'id', type: 'INT', checked: true},
      ]);
      leftCols[0].alias = 'left_id';

      const rightCols = createCheckedColumns([
        {name: 'id', type: 'INT', checked: true},
      ]);
      rightCols[0].alias = 'right_id';

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '',
          leftColumns: leftCols,
          rightColumns: rightCols,
        },
        {},
      );

      const finalCols = joinNode.finalCols;

      expect(finalCols.length).toBe(2);
      expect(finalCols[0].alias).toBe('left_id');
      expect(finalCols[1].alias).toBe('right_id');
    });
  });

  describe('validation', () => {
    it('should fail when only one node is provided', () => {
      const node1 = createMockPrevNode('node1', []);

      const joinNode = createJoinNodeWithInputs(
        node1,
        undefined,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      expect(joinNode.validate()).toBe(false);
      expect(joinNode.context.issues?.queryError?.message).toContain(
        'exactly two sources',
      );
    });

    it('should pass when aliases are set by constructor defaults and columns selected', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
      ]);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: undefined!,
          rightQueryAlias: undefined!,
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '', // Provide checked columns for validation to pass
          leftColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: true},
          ]),
          rightColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: false},
          ]),
        },
        {},
      );

      // Constructor sets default aliases to 'left' and 'right', so this should pass
      expect(joinNode.validate()).toBe(true);
    });

    it('should fail when equality columns are missing in equality mode', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: '',
          rightColumn: '',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      expect(joinNode.validate()).toBe(false);
      expect(joinNode.context.issues?.queryError?.message).toContain(
        'Both left and right columns are required',
      );
    });

    it('should fail when SQL expression is missing in freeform mode', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'freeform',
          joinType: 'INNER',
          leftColumn: '',
          rightColumn: '',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      expect(joinNode.validate()).toBe(false);
      expect(joinNode.context.issues?.queryError?.message).toContain(
        'SQL expression',
      );
    });

    it('should pass validation with valid equality condition and checked columns', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
      ]);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '', // Provide at least one checked column
          leftColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: true},
          ]),
          rightColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: false},
          ]),
        },
        {},
      );

      expect(joinNode.validate()).toBe(true);
    });

    it('should fail when no columns are checked', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('value', 'INT'),
      ]);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '', // All columns unchecked
          leftColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: false},
            {name: 'name', type: 'STRING', checked: false},
          ]),
          rightColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: false},
            {name: 'value', type: 'INT', checked: false},
          ]),
        },
        {},
      );

      expect(joinNode.validate()).toBe(false);
      expect(joinNode.context.issues?.queryError?.message).toContain(
        'No columns selected',
      );
    });

    it('should fail when columns default to unchecked', () => {
      // When leftColumns/rightColumns are undefined, columns default to unchecked
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('value', 'INT'),
      ]);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      expect(joinNode.validate()).toBe(false);
      expect(joinNode.context.issues?.queryError?.message).toContain(
        'No columns selected',
      );
    });

    it('should pass validation with valid freeform condition and checked columns', () => {
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('parent_id', 'INT'),
      ]);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 't1',
          rightQueryAlias: 't2',
          conditionType: 'freeform',
          joinType: 'INNER',
          leftColumn: '',
          rightColumn: '',
          sqlExpression: 't1.id = t2.parent_id', // Provide at least one checked column
          leftColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: true},
          ]),
          rightColumns: createCheckedColumns([
            {name: 'parent_id', type: 'INT', checked: true},
          ]),
        },
        {},
      );

      expect(joinNode.validate()).toBe(true);
    });
  });

  describe('getTitle', () => {
    it('should return "Join"', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: '',
          rightColumn: '',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      expect(joinNode.getTitle()).toBe('Join');
    });
  });

  describe('secondaryInputs.portNames', () => {
    it('should return the left and right query aliases', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'foo',
          rightQueryAlias: 'bar',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: '',
          rightColumn: '',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      const portNames = joinNode.secondaryInputs.portNames;
      expect(typeof portNames).toBe('function');
      if (typeof portNames === 'function') {
        expect(portNames(0)).toBe('foo');
        expect(portNames(1)).toBe('bar');
      }
    });
  });

  describe('clone', () => {
    it('should create a deep copy of the node', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      const cloned = joinNode.clone() as JoinNode;

      expect(cloned).not.toBe(joinNode);
      expect(cloned.attrs.leftQueryAlias).toBe('left');
      expect(cloned.attrs.rightQueryAlias).toBe('right');
      expect(cloned.attrs.conditionType).toBe('equality');
      expect(cloned.attrs.leftColumn).toBe('id');
      expect(cloned.attrs.rightColumn).toBe('id');
    });

    it('should not share state with original', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      const cloned = joinNode.clone() as JoinNode;

      // Modify the cloned state
      cloned.attrs.leftQueryAlias = 'modified';

      // Original should not be affected
      expect(joinNode.attrs.leftQueryAlias).toBe('left');
    });
  });

  describe('getStructuredQuery', () => {
    it('should return undefined if validation fails', () => {
      const node1 = createMockPrevNode('node1', []);

      const joinNode = createJoinNodeWithInputs(
        node1,
        undefined,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: '',
          rightColumn: '',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      expect(joinNode.getStructuredQuery()).toBeUndefined();
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

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '', // Provide checked columns
          leftColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: true},
            {name: 'name', type: 'STRING', checked: true},
          ]),
          rightColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: false},
            {name: 'value', type: 'INT', checked: true},
          ]),
        },
        {},
      );

      const sq = joinNode.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.selectColumns).toBeDefined();

      const selectColNames = sq?.selectColumns?.map(
        (c) => c.columnNameOrExpression,
      );
      const finalColNames = joinNode.finalCols.map((c: ColumnInfo) => c.name);

      expect(selectColNames).toEqual(finalColNames);
      expect(selectColNames).toEqual(['id', 'name', 'value']);
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

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '', // Provide checked columns
          leftColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: true},
          ]),
          rightColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: false},
          ]),
        },
        {},
      );

      const sq = joinNode.getStructuredQuery();

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

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 't1',
          rightQueryAlias: 't2',
          conditionType: 'freeform',
          joinType: 'INNER',
          leftColumn: '',
          rightColumn: '',
          sqlExpression: 't1.id = t2.parent_id', // Provide checked columns
          leftColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: true},
          ]),
          rightColumns: createCheckedColumns([
            {name: 'parent_id', type: 'INT', checked: true},
          ]),
        },
        {},
      );

      const sq = joinNode.getStructuredQuery();

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

  describe('attrs serialization', () => {
    it('should have all state fields in attrs', () => {
      const node1 = createMockPrevNode('node1', []);
      const node2 = createMockPrevNode('node2', []);

      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '',
          leftColumns: undefined,
          rightColumns: undefined,
        },
        {},
      );

      expect(joinNode.attrs.leftQueryAlias).toBe('left');
      expect(joinNode.attrs.rightQueryAlias).toBe('right');
      expect(joinNode.attrs.conditionType).toBe('equality');
      expect(joinNode.attrs.leftColumn).toBe('id');
      expect(joinNode.attrs.rightColumn).toBe('id');
    });
  });

  describe('deserializeState', () => {
    it('should deserialize state correctly', () => {
      const serialized = {
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality' as const,
        joinType: 'INNER' as const,
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
        leftColumns: undefined,
        rightColumns: undefined,
      };

      const state = JoinNode.deserializeState(serialized);

      expect(state.leftQueryAlias).toBe('left');
      expect(state.rightQueryAlias).toBe('right');
      expect(state.conditionType).toBe('equality');
      expect(state.leftColumn).toBe('id');
      expect(state.rightColumn).toBe('id');
    });

    it('should deserialize legacy string types into PerfettoSqlType', () => {
      const legacySerialized = {
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality' as const,
        leftColumn: 'id',
        rightColumn: 'id',
        leftColumns: [
          {name: 'id', type: 'INT', checked: true, columnName: 'id'},
          {name: 'ts', type: 'TIMESTAMP', checked: false, columnName: 'ts'},
        ],
        rightColumns: [
          {name: 'val', type: 'DOUBLE', checked: true, columnName: 'val'},
        ],
      } as unknown as JoinNodeAttrs;

      const state = JoinNode.deserializeState(legacySerialized);

      expect(state.leftColumns?.[0].type).toEqual(PerfettoSqlTypes.INT);
      expect(state.leftColumns?.[1].type).toEqual(PerfettoSqlTypes.TIMESTAMP);
      expect(state.rightColumns?.[0].type).toEqual(PerfettoSqlTypes.DOUBLE);
    });

    it('should deserialize new PerfettoSqlType objects correctly', () => {
      const newSerialized: JoinNodeAttrs = {
        leftQueryAlias: 'left',
        rightQueryAlias: 'right',
        conditionType: 'equality',
        joinType: 'INNER',
        leftColumn: 'id',
        rightColumn: 'id',
        sqlExpression: '',
        leftColumns: [{name: 'id', type: {kind: 'int'}, checked: true}],
        rightColumns: [{name: 'dur', type: {kind: 'duration'}, checked: true}],
      };

      const state = JoinNode.deserializeState(newSerialized);

      expect(state.leftColumns?.[0].type).toEqual(PerfettoSqlTypes.INT);
      expect(state.rightColumns?.[0].type).toEqual(PerfettoSqlTypes.DURATION);
    });
  });

  describe('serialize/deserialize round-trip', () => {
    it('should preserve checked columns after serialization and deserialization', () => {
      // Create mock source nodes with columns
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
        createColumnInfo('name', 'STRING'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('value', 'INT'),
      ]);

      // Create a join node with specific columns checked
      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'value',
          sqlExpression: '',
          leftColumns: createCheckedColumns([
            {name: 'id', type: 'INT', checked: true},
            {name: 'name', type: 'STRING', checked: false},
          ]),
          rightColumns: createCheckedColumns([
            {name: 'value', type: 'INT', checked: true},
          ]),
        },
        {},
      );

      // Verify initial state
      expect(joinNode.attrs.leftColumns?.[0].checked).toBe(true);
      expect(joinNode.attrs.leftColumns?.[1].checked).toBe(false);
      expect(joinNode.attrs.rightColumns?.[0].checked).toBe(true);

      // Serialize the state via attrs
      const serialized = joinNode.attrs;

      // Deserialize the state
      const deserializedState = JoinNode.deserializeState(serialized);

      // Create a new join node from deserialized state
      // Connections are now restored at the graph level by json_handler
      const restoredNode = createJoinNodeWithInputs(
        node1,
        node2,
        {...deserializedState},
        {},
      );

      expect(restoredNode.attrs.leftColumns?.length).toBe(2);
      expect(restoredNode.attrs.rightColumns?.length).toBe(1);

      expect(restoredNode.attrs.leftColumns?.[0].checked).toBe(true);
      expect(restoredNode.attrs.leftColumns?.[1].checked).toBe(false);
      expect(restoredNode.attrs.rightColumns?.[0].checked).toBe(true);
    });

    it('should preserve column aliases after serialization and deserialization', () => {
      // Create mock source nodes with columns
      const node1 = createMockPrevNode('node1', [
        createColumnInfo('id', 'INT'),
      ]);
      const node2 = createMockPrevNode('node2', [
        createColumnInfo('id', 'INT'),
      ]);

      // Create columns with aliases
      const leftCols = createCheckedColumns([
        {name: 'id', type: 'INT', checked: true},
      ]);
      leftCols[0].alias = 'left_id';

      const rightCols = createCheckedColumns([
        {name: 'id', type: 'INT', checked: true},
      ]);
      rightCols[0].alias = 'right_id';

      // Create a join node with aliases
      const joinNode = createJoinNodeWithInputs(
        node1,
        node2,
        {
          leftQueryAlias: 'left',
          rightQueryAlias: 'right',
          conditionType: 'equality',
          joinType: 'INNER',
          leftColumn: 'id',
          rightColumn: 'id',
          sqlExpression: '',
          leftColumns: leftCols,
          rightColumns: rightCols,
        },
        {},
      );

      // Verify initial state has aliases
      expect(joinNode.attrs.leftColumns?.[0].alias).toBe('left_id');
      expect(joinNode.attrs.rightColumns?.[0].alias).toBe('right_id');

      // Serialize the state via attrs
      const serialized = joinNode.attrs;

      // Deserialize the state
      const deserializedState = JoinNode.deserializeState(serialized);

      // Create a new join node from deserialized state
      // Connections are now restored at the graph level by json_handler
      const restoredNode = createJoinNodeWithInputs(
        node1,
        node2,
        {...deserializedState},
        {},
      );

      expect(restoredNode.attrs.leftColumns?.[0].alias).toBe('left_id');
      expect(restoredNode.attrs.rightColumns?.[0].alias).toBe('right_id');
      expect(restoredNode.attrs.leftColumns?.[0].checked).toBe(true);
      expect(restoredNode.attrs.rightColumns?.[0].checked).toBe(true);
    });
  });
});
