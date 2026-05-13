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

import {CreateSlicesNode} from './create_slices_node';
import {QueryNode} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {
  createMockNode,
  createColumnInfo,
  createMockStructuredQuery,
  createNodeIssuesWithQueryError,
  ColumnType,
} from '../testing/test_utils';

describe('CreateSlicesNode', () => {
  function createMockPrevNode(id: string, columns: ColumnInfo[]): QueryNode {
    return createMockNode({
      nodeId: id,
      columns,
      getTitle: () => `Mock ${id}`,
    });
  }

  function createCol(
    name: string,
    type: ColumnType,
    checked = true,
  ): ColumnInfo {
    return createColumnInfo(name, type, {checked});
  }

  describe('constructor', () => {
    it('should initialize with default values', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('ts', 'timestamp'),
        createCol('name', 'string'),
      ]);
      const endsNode = createMockPrevNode('ends', [
        createCol('ts', 'timestamp'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.attrs.startsTsColumn).toBe('ts');
      expect(createSlicesNode.attrs.endsTsColumn).toBe('ts');
      expect(createSlicesNode.context.autoExecute).toBeUndefined();
    });

    it('should use default timestamp columns when not provided', () => {
      const startsNode = createMockPrevNode('starts', []);
      const endsNode = createMockPrevNode('ends', []);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: undefined!,
          endsTsColumn: undefined!,
        },
        {},
      );

      expect(createSlicesNode.attrs.startsTsColumn).toBe('ts');
      expect(createSlicesNode.attrs.endsTsColumn).toBe('ts');
    });

    it('should accept custom timestamp column names', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('acquire_ts', 'timestamp'),
      ]);
      const endsNode = createMockPrevNode('ends', [
        createCol('release_ts', 'timestamp'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'acquire_ts',
          endsTsColumn: 'release_ts',
        },
        {},
      );

      expect(createSlicesNode.attrs.startsTsColumn).toBe('acquire_ts');
      expect(createSlicesNode.attrs.endsTsColumn).toBe('release_ts');
    });
  });

  describe('finalCols', () => {
    it('should return empty array when only starts node is provided', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('ts', 'timestamp'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode: undefined,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.finalCols).toEqual([]);
    });

    it('should return empty array when only ends node is provided', () => {
      const endsNode = createMockPrevNode('ends', [
        createCol('ts', 'timestamp'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode: undefined,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.finalCols).toEqual([]);
    });

    it('should return ts and dur columns when both nodes are provided', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('ts', 'timestamp'),
      ]);
      const endsNode = createMockPrevNode('ends', [
        createCol('ts', 'timestamp'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      const finalCols = createSlicesNode.finalCols;

      expect(finalCols.length).toBe(2);
      expect(finalCols[0].name).toBe('ts');
      expect(finalCols[0].type).toEqual({kind: 'timestamp'});
      expect(finalCols[0].checked).toBe(true);
      expect(finalCols[1].name).toBe('dur');
      expect(finalCols[1].type).toEqual({kind: 'duration'});
      expect(finalCols[1].checked).toBe(true);
    });

    it('should always return the same ts and dur columns regardless of input columns', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('acquire_ts', 'timestamp'),
        createCol('lock_id', 'int'),
        createCol('thread_name', 'string'),
      ]);
      const endsNode = createMockPrevNode('ends', [
        createCol('release_ts', 'timestamp'),
        createCol('lock_id', 'int'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'acquire_ts',
          endsTsColumn: 'release_ts',
        },
        {},
      );

      const finalCols = createSlicesNode.finalCols;

      expect(finalCols.length).toBe(2);
      expect(finalCols.map((c) => c.name)).toEqual(['ts', 'dur']);
    });
  });

  describe('validation', () => {
    it('should fail when only starts node is provided', () => {
      const startsNode = createMockPrevNode('starts', []);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode: undefined,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.validate()).toBe(false);
      expect(createSlicesNode.context.issues?.queryError?.message).toContain(
        'exactly two sources',
      );
    });

    it('should fail when only ends node is provided', () => {
      const endsNode = createMockPrevNode('ends', []);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode: undefined,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.validate()).toBe(false);
      expect(createSlicesNode.context.issues?.queryError?.message).toContain(
        'exactly two sources',
      );
    });

    it('should fail when starts timestamp column is empty', () => {
      const startsNode = createMockPrevNode('starts', []);
      const endsNode = createMockPrevNode('ends', []);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: '',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.validate()).toBe(false);
      expect(createSlicesNode.context.issues?.queryError?.message).toContain(
        'Starts timestamp column is required',
      );
    });

    it('should fail when ends timestamp column is empty', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('ts', 'timestamp'),
      ]);
      const endsNode = createMockPrevNode('ends', []);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: '',
        },
        {},
      );

      expect(createSlicesNode.validate()).toBe(false);
      expect(createSlicesNode.context.issues?.queryError?.message).toContain(
        'Ends timestamp column is required',
      );
    });

    it('should fail when starts node validation fails', () => {
      const startsNode = createMockNode({
        nodeId: 'starts',
        columns: [createCol('ts', 'timestamp')],
        validate: () => false,
        context: {
          issues: createNodeIssuesWithQueryError('Starts node has errors'),
        },
      });
      const endsNode = createMockPrevNode('ends', [
        createCol('ts', 'timestamp'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.validate()).toBe(false);
      expect(createSlicesNode.context.issues?.queryError?.message).toContain(
        'Starts node has errors',
      );
    });

    it('should fail when ends node validation fails', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('ts', 'timestamp'),
      ]);
      const endsNode = createMockNode({
        nodeId: 'ends',
        columns: [createCol('ts', 'timestamp')],
        validate: () => false,
        context: {
          issues: createNodeIssuesWithQueryError('Ends node has errors'),
        },
      });

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.validate()).toBe(false);
      expect(createSlicesNode.context.issues?.queryError?.message).toContain(
        'Ends node has errors',
      );
    });

    it('should fail when starts timestamp column does not exist in starts node', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('other_column', 'int'),
      ]);
      const endsNode = createMockPrevNode('ends', [
        createCol('ts', 'timestamp'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.validate()).toBe(false);
      expect(createSlicesNode.context.issues?.queryError?.message).toContain(
        "Starts timestamp column 'ts' not found",
      );
    });

    it('should fail when ends timestamp column does not exist in ends node', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('ts', 'timestamp'),
      ]);
      const endsNode = createMockPrevNode('ends', [
        createCol('other_column', 'int'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.validate()).toBe(false);
      expect(createSlicesNode.context.issues?.queryError?.message).toContain(
        "Ends timestamp column 'ts' not found",
      );
    });

    it('should pass validation with valid inputs', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('ts', 'timestamp'),
      ]);
      const endsNode = createMockPrevNode('ends', [
        createCol('ts', 'timestamp'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.validate()).toBe(true);
    });

    it('should pass validation with custom timestamp columns', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('acquire_ts', 'timestamp'),
        createCol('lock_id', 'int'),
      ]);
      const endsNode = createMockPrevNode('ends', [
        createCol('release_ts', 'timestamp'),
        createCol('lock_id', 'int'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'acquire_ts',
          endsTsColumn: 'release_ts',
        },
        {},
      );

      expect(createSlicesNode.validate()).toBe(true);
    });
  });

  describe('getTitle', () => {
    it('should return "Create Slices"', () => {
      const startsNode = createMockPrevNode('starts', []);
      const endsNode = createMockPrevNode('ends', []);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.getTitle()).toBe('Create Slices');
    });
  });

  describe('getInputLabels', () => {
    it('should return "Starts" and "Ends"', () => {
      const startsNode = createMockPrevNode('starts', []);
      const endsNode = createMockPrevNode('ends', []);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.getInputLabels()).toEqual(['Starts', 'Ends']);
    });
  });

  describe('clone', () => {
    it('should create a deep copy of the node', () => {
      const startsNode = createMockPrevNode('starts', []);
      const endsNode = createMockPrevNode('ends', []);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'acquire_ts',
          endsTsColumn: 'release_ts',
        },
        {},
      );

      const cloned = createSlicesNode.clone() as CreateSlicesNode;

      expect(cloned).not.toBe(createSlicesNode);
      expect(cloned.attrs.startsTsColumn).toBe('acquire_ts');
      expect(cloned.attrs.endsTsColumn).toBe('release_ts');
    });

    it('should not share state with original', () => {
      const startsNode = createMockPrevNode('starts', []);
      const endsNode = createMockPrevNode('ends', []);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      const cloned = createSlicesNode.clone() as CreateSlicesNode;

      // Modify the cloned state
      cloned.attrs.startsTsColumn = 'modified';

      // Original should not be affected
      expect(createSlicesNode.attrs.startsTsColumn).toBe('ts');
    });

    it('should not share connections with original', () => {
      const startsNode = createMockPrevNode('starts', []);
      const endsNode = createMockPrevNode('ends', []);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      const cloned = createSlicesNode.clone() as CreateSlicesNode;

      // Cloned node should have no connections
      expect(cloned.startsNode).toBeUndefined();
      expect(cloned.endsNode).toBeUndefined();
      expect(cloned.secondaryInputs.connections.size).toBe(0);

      // Original should still have connections
      expect(createSlicesNode.startsNode).toBe(startsNode);
      expect(createSlicesNode.endsNode).toBe(endsNode);
      expect(createSlicesNode.secondaryInputs.connections.size).toBe(2);
    });
  });

  describe('getStructuredQuery', () => {
    it('should return undefined if validation fails', () => {
      const startsNode = createMockPrevNode('starts', []);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode: undefined,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.getStructuredQuery()).toBeUndefined();
    });

    it('should return undefined if starts node has no structured query', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('ts', 'timestamp'),
      ]);
      const endsNode = createMockNode({
        nodeId: 'ends',
        columns: [createCol('ts', 'timestamp')],
        getStructuredQuery: () => new protos.PerfettoSqlStructuredQuery(),
      });

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.getStructuredQuery()).toBeUndefined();
    });

    it('should return undefined if ends node has no structured query', () => {
      const startsNode = createMockNode({
        nodeId: 'starts',
        columns: [createCol('ts', 'timestamp')],
        getStructuredQuery: () => new protos.PerfettoSqlStructuredQuery(),
      });
      const endsNode = createMockPrevNode('ends', [
        createCol('ts', 'timestamp'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      expect(createSlicesNode.getStructuredQuery()).toBeUndefined();
    });

    it('should create ExperimentalCreateSlices structured query', () => {
      const mockSq1 = createMockStructuredQuery();
      const mockSq2 = createMockStructuredQuery();

      const startsNode = createMockNode({
        nodeId: 'starts',
        columns: [createCol('ts', 'timestamp')],
        getStructuredQuery: () => mockSq1,
      });

      const endsNode = createMockNode({
        nodeId: 'ends',
        columns: [createCol('ts', 'timestamp')],
        getStructuredQuery: () => mockSq2,
      });

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      const sq = createSlicesNode.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.experimentalCreateSlices).toBeDefined();
      expect(sq?.experimentalCreateSlices?.startsQuery).toBe(mockSq1);
      expect(sq?.experimentalCreateSlices?.endsQuery).toBe(mockSq2);
      expect(sq?.experimentalCreateSlices?.startsTsColumn).toBe('ts');
      expect(sq?.experimentalCreateSlices?.endsTsColumn).toBe('ts');
    });

    it('should use custom timestamp column names', () => {
      const mockSq1 = createMockStructuredQuery();
      const mockSq2 = createMockStructuredQuery();

      const startsNode = createMockNode({
        nodeId: 'starts',
        columns: [createCol('acquire_ts', 'timestamp')],
        getStructuredQuery: () => mockSq1,
      });

      const endsNode = createMockNode({
        nodeId: 'ends',
        columns: [createCol('release_ts', 'timestamp')],
        getStructuredQuery: () => mockSq2,
      });

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'acquire_ts',
          endsTsColumn: 'release_ts',
        },
        {},
      );

      const sq = createSlicesNode.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.experimentalCreateSlices?.startsTsColumn).toBe('acquire_ts');
      expect(sq?.experimentalCreateSlices?.endsTsColumn).toBe('release_ts');
    });

    it('should set the node id on the structured query', () => {
      const mockSq1 = createMockStructuredQuery();
      const mockSq2 = createMockStructuredQuery();

      const startsNode = createMockNode({
        nodeId: 'starts',
        columns: [createCol('ts', 'timestamp')],
        getStructuredQuery: () => mockSq1,
      });

      const endsNode = createMockNode({
        nodeId: 'ends',
        columns: [createCol('ts', 'timestamp')],
        getStructuredQuery: () => mockSq2,
      });

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      const sq = createSlicesNode.getStructuredQuery();

      expect(sq?.id).toBe(createSlicesNode.nodeId);
    });

    it('should handle ts_dur mode for starts input', () => {
      // Mock queries need IDs for reference-based query building
      const mockSq1 = createMockStructuredQuery('starts_mock');
      const mockSq2 = createMockStructuredQuery('ends_mock');

      const startsNode = createMockNode({
        nodeId: 'starts',
        columns: [createCol('ts', 'timestamp'), createCol('dur', 'duration')],
        getStructuredQuery: () => mockSq1,
      });

      const endsNode = createMockNode({
        nodeId: 'ends',
        columns: [createCol('ts', 'timestamp')],
        getStructuredQuery: () => mockSq2,
      });

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsMode: 'ts_dur',
          endsMode: 'ts',
          startsTsColumn: 'ts',
          startsDurColumn: 'dur',
          endsTsColumn: 'ts',
        },
        {},
      );

      const sq = createSlicesNode.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.experimentalCreateSlices).toBeDefined();
      // Should use computed column name for starts
      expect(sq?.experimentalCreateSlices?.startsTsColumn).toBe(
        'exp_tmp_starts_computed_end_ts',
      );
      // Should use original column name for ends
      expect(sq?.experimentalCreateSlices?.endsTsColumn).toBe('ts');
      // The starts query should be wrapped with a select that includes the computed column
      expect(sq?.experimentalCreateSlices?.startsQuery).toBeDefined();
      expect(sq?.experimentalCreateSlices?.startsQuery?.id).toBe(
        `${createSlicesNode.nodeId}_starts_computed`,
      );
    });

    it('should handle ts_dur mode for ends input', () => {
      // Mock queries need IDs for reference-based query building
      const mockSq1 = createMockStructuredQuery('starts_mock');
      const mockSq2 = createMockStructuredQuery('ends_mock');

      const startsNode = createMockNode({
        nodeId: 'starts',
        columns: [createCol('ts', 'timestamp')],
        getStructuredQuery: () => mockSq1,
      });

      const endsNode = createMockNode({
        nodeId: 'ends',
        columns: [createCol('ts', 'timestamp'), createCol('dur', 'duration')],
        getStructuredQuery: () => mockSq2,
      });

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsMode: 'ts',
          endsMode: 'ts_dur',
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
          endsDurColumn: 'dur',
        },
        {},
      );

      const sq = createSlicesNode.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.experimentalCreateSlices).toBeDefined();
      // Should use original column name for starts
      expect(sq?.experimentalCreateSlices?.startsTsColumn).toBe('ts');
      // Should use computed column name for ends
      expect(sq?.experimentalCreateSlices?.endsTsColumn).toBe(
        'exp_tmp_ends_computed_end_ts',
      );
      // The ends query should be wrapped with a select that includes the computed column
      expect(sq?.experimentalCreateSlices?.endsQuery).toBeDefined();
      expect(sq?.experimentalCreateSlices?.endsQuery?.id).toBe(
        `${createSlicesNode.nodeId}_ends_computed`,
      );
    });

    it('should handle ts_dur mode for both inputs', () => {
      // Mock queries need IDs for reference-based query building
      const mockSq1 = createMockStructuredQuery('starts_mock');
      const mockSq2 = createMockStructuredQuery('ends_mock');

      const startsNode = createMockNode({
        nodeId: 'starts',
        columns: [createCol('ts', 'timestamp'), createCol('dur', 'duration')],
        getStructuredQuery: () => mockSq1,
      });

      const endsNode = createMockNode({
        nodeId: 'ends',
        columns: [createCol('ts', 'timestamp'), createCol('dur', 'duration')],
        getStructuredQuery: () => mockSq2,
      });

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsMode: 'ts_dur',
          endsMode: 'ts_dur',
          startsTsColumn: 'ts',
          startsDurColumn: 'dur',
          endsTsColumn: 'ts',
          endsDurColumn: 'dur',
        },
        {},
      );

      const sq = createSlicesNode.getStructuredQuery();

      expect(sq).toBeDefined();
      expect(sq?.experimentalCreateSlices).toBeDefined();
      // Should use computed column names for both
      expect(sq?.experimentalCreateSlices?.startsTsColumn).toBe(
        'exp_tmp_starts_computed_end_ts',
      );
      expect(sq?.experimentalCreateSlices?.endsTsColumn).toBe(
        'exp_tmp_ends_computed_end_ts',
      );
      // Both queries should be wrapped
      expect(sq?.experimentalCreateSlices?.startsQuery?.id).toBe(
        `${createSlicesNode.nodeId}_starts_computed`,
      );
      expect(sq?.experimentalCreateSlices?.endsQuery?.id).toBe(
        `${createSlicesNode.nodeId}_ends_computed`,
      );
    });
  });

  describe('serializeState', () => {
    it('should serialize all state fields', () => {
      const startsNode = createMockPrevNode('starts_node_id', []);
      const endsNode = createMockPrevNode('ends_node_id', []);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: 'acquire_ts',
          endsTsColumn: 'release_ts',
        },
        {},
      );

      const serialized = createSlicesNode.attrs;

      expect(serialized.startsTsColumn).toBe('acquire_ts');
      expect(serialized.endsTsColumn).toBe('release_ts');
    });

    it('should handle empty node IDs', () => {
      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode: undefined,
          endsNode: undefined,
          startsTsColumn: 'ts',
          endsTsColumn: 'ts',
        },
        {},
      );

      const serialized = createSlicesNode.attrs;

      expect(serialized.startsTsColumn).toBe('ts');
      expect(serialized.endsTsColumn).toBe('ts');
    });
  });

  describe('deserialize', () => {
    it('should restore state via constructor', () => {
      const node = new CreateSlicesNode(
        {startsTsColumn: 'acquire_ts', endsTsColumn: 'release_ts'},
        {},
      );

      expect(node.attrs.startsTsColumn).toBe('acquire_ts');
      expect(node.attrs.endsTsColumn).toBe('release_ts');
    });

    it('should use default values when fields are missing', () => {
      const node = new CreateSlicesNode(
        {startsTsColumn: undefined!, endsTsColumn: undefined!},
        {},
      );

      expect(node.attrs.startsTsColumn).toBe('ts');
      expect(node.attrs.endsTsColumn).toBe('ts');
    });
  });

  describe('auto-selection based on column type', () => {
    it('should auto-select timestamp column based on type, not name', () => {
      // Create a node with a single timestamp column with a custom name
      const startsNode = createMockPrevNode('starts', [
        createCol('custom_timestamp', 'timestamp'),
        createCol('name', 'string'),
      ]);
      const endsNode = createMockPrevNode('ends', [
        createCol('another_ts_name', 'timestamp'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsTsColumn: '', // Empty to trigger auto-selection
          endsTsColumn: '', // Empty to trigger auto-selection
        },
        {},
      );

      // Validation should pass because there's only one timestamp column
      // and it should be auto-selected based on TYPE, not name
      expect(createSlicesNode.validate()).toBe(true);
      expect(createSlicesNode.attrs.startsTsColumn).toBe('custom_timestamp');
      expect(createSlicesNode.attrs.endsTsColumn).toBe('another_ts_name');
    });

    it('should auto-select duration column based on type, not name', () => {
      const startsNode = createMockPrevNode('starts', [
        createCol('my_ts', 'timestamp'),
        createCol('my_duration', 'duration'),
      ]);
      const endsNode = createMockPrevNode('ends', [
        createCol('end_ts', 'timestamp'),
        createCol('end_duration', 'duration'),
      ]);

      const createSlicesNode = new CreateSlicesNode(
        {
          startsNode,
          endsNode,
          startsMode: 'ts_dur',
          endsMode: 'ts_dur',
          startsTsColumn: '', // Empty to trigger auto-selection
          endsTsColumn: '', // Empty to trigger auto-selection
          startsDurColumn: '', // Empty to trigger auto-selection
          endsDurColumn: '', // Empty to trigger auto-selection
        },
        {},
      );

      // Validation should pass and auto-select based on type
      expect(createSlicesNode.validate()).toBe(true);
      expect(createSlicesNode.attrs.startsTsColumn).toBe('my_ts');
      expect(createSlicesNode.attrs.endsTsColumn).toBe('end_ts');
      expect(createSlicesNode.attrs.startsDurColumn).toBe('my_duration');
      expect(createSlicesNode.attrs.endsDurColumn).toBe('end_duration');
    });
  });
});
