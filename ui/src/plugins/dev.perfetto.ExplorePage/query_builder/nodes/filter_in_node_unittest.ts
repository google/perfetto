// Copyright (C) 2026 The Android Open Source Project
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

import {FilterInNode} from './filter_in_node';
import {NodeType} from '../../query_node';
import {
  createMockNode,
  createColumnInfo,
  connectNodes,
  connectSecondary,
  expectValidationError,
  expectValidationSuccess,
  createMockNodeWithStructuredQuery,
} from '../testing/test_utils';

// Interface for accessing private methods during testing
interface FilterInNodeWithPrivates {
  autoSuggestColumns(): void;
  cleanupStaleColumns(): void;
}

describe('FilterInNode', () => {
  describe('constructor', () => {
    it('should have correct node type', () => {
      const node = new FilterInNode({});
      expect(node.type).toBe(NodeType.kFilterIn);
    });

    it('should initialize with no inputs', () => {
      const node = new FilterInNode({});
      expect(node.primaryInput).toBeUndefined();
      expect(node.matchValuesNode).toBeUndefined();
    });

    it('should have one secondary input port named Input', () => {
      const node = new FilterInNode({});
      expect(node.secondaryInputs.min).toBe(1);
      expect(node.secondaryInputs.max).toBe(1);
      expect(node.secondaryInputs.portNames).toEqual(['Input']);
    });

    it('should preserve state from constructor', () => {
      const node = new FilterInNode({
        baseColumn: 'utid',
        matchColumn: 'id',
      });
      expect(node.state.baseColumn).toBe('utid');
      expect(node.state.matchColumn).toBe('id');
    });
  });

  describe('finalCols', () => {
    it('should return empty array when no primary input', () => {
      const node = new FilterInNode({});
      expect(node.finalCols).toEqual([]);
    });

    it('should return same columns as primary input', () => {
      const primaryCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('name', 'string'),
        createColumnInfo('utid', 'int'),
      ];
      const primaryNode = createMockNode({columns: primaryCols});

      const node = new FilterInNode({});
      node.primaryInput = primaryNode;

      expect(node.finalCols).toEqual(primaryCols);
    });

    it('should not include columns from match values node', () => {
      const primaryCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('name', 'string'),
      ];
      const matchCols = [
        createColumnInfo('id', 'int'),
        createColumnInfo('extra_col', 'string'),
      ];
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: primaryCols,
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: matchCols,
      });

      const node = new FilterInNode({baseColumn: 'id', matchColumn: 'id'});
      connectNodes(primaryNode, node);
      connectSecondary(matchNode, node, 0);

      expect(node.finalCols).toEqual(primaryCols);
    });
  });

  describe('validate', () => {
    it('should fail when no primary input', () => {
      const node = new FilterInNode({});
      expectValidationError(node, 'Connect a node with rows to filter');
    });

    it('should fail when primary input is invalid', () => {
      const primaryNode = createMockNode({validate: () => false});
      const matchNode = createMockNode({nodeId: 'match'});

      const node = new FilterInNode({baseColumn: 'id', matchColumn: 'id'});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      expectValidationError(node, 'Primary input is invalid');
    });

    it('should fail when no secondary input', () => {
      const primaryNode = createMockNode({
        columns: [createColumnInfo('id', 'int')],
      });

      const node = new FilterInNode({baseColumn: 'id', matchColumn: 'id'});
      node.primaryInput = primaryNode;

      expectValidationError(node, 'Connect a node with match values');
    });

    it('should fail when secondary input is invalid', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [createColumnInfo('id', 'int')],
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        validate: () => false,
      });

      const node = new FilterInNode({baseColumn: 'id', matchColumn: 'id'});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      expectValidationError(node, 'Input node is invalid');
    });

    it('should fail when base column not specified', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [createColumnInfo('id', 'int')],
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: [createColumnInfo('id', 'int')],
      });

      const node = new FilterInNode({matchColumn: 'id'});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      expectValidationError(node, 'Select a base column');
    });

    it('should fail when match column not specified', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [createColumnInfo('id', 'int')],
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: [createColumnInfo('id', 'int')],
      });

      const node = new FilterInNode({baseColumn: 'id'});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      expectValidationError(node, 'Select a match column');
    });

    it('should fail when base column does not exist in primary input', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [createColumnInfo('name', 'string')],
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: [createColumnInfo('id', 'int')],
      });

      const node = new FilterInNode({
        baseColumn: 'utid',
        matchColumn: 'id',
      });
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      expectValidationError(node, 'Primary input is missing column: utid');
    });

    it('should fail when match column does not exist in match values input', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [createColumnInfo('utid', 'int')],
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: [createColumnInfo('name', 'string')],
      });

      const node = new FilterInNode({
        baseColumn: 'utid',
        matchColumn: 'thread_id',
      });
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      expectValidationError(node, 'Input node is missing column: thread_id');
    });

    it('should pass when all requirements met', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('utid', 'int'),
          createColumnInfo('name', 'string'),
        ],
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: [createColumnInfo('thread_id', 'int')],
      });

      const node = new FilterInNode({
        baseColumn: 'utid',
        matchColumn: 'thread_id',
      });
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      expectValidationSuccess(node);
    });

    it('should pass when base and match columns have different names', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [createColumnInfo('track_id', 'int')],
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: [createColumnInfo('id', 'int')],
      });

      const node = new FilterInNode({
        baseColumn: 'track_id',
        matchColumn: 'id',
      });
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      expectValidationSuccess(node);
    });
  });

  describe('getStructuredQuery', () => {
    it('should return undefined when validation fails', () => {
      const node = new FilterInNode({});
      expect(node.getStructuredQuery()).toBeUndefined();
    });

    it('should return structured query when valid', () => {
      const primaryNode = createMockNodeWithStructuredQuery('primary', [
        createColumnInfo('id', 'int'),
        createColumnInfo('utid', 'int'),
      ]);
      const matchNode = createMockNodeWithStructuredQuery('match', [
        createColumnInfo('id', 'int'),
      ]);

      const node = new FilterInNode({
        baseColumn: 'utid',
        matchColumn: 'id',
      });
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      const sq = node.getStructuredQuery();
      expect(sq).toBeDefined();
      expect(sq?.id).toBe(node.nodeId);
      expect(sq?.experimentalFilterIn).toBeDefined();
      expect(sq?.experimentalFilterIn?.baseColumn).toBe('utid');
      expect(sq?.experimentalFilterIn?.matchColumn).toBe('id');
    });
  });

  describe('autoSuggestColumns', () => {
    it('should auto-suggest when exactly one common column', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('utid', 'int'),
          createColumnInfo('name', 'string'),
        ],
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: [
          createColumnInfo('utid', 'int'),
          createColumnInfo('thread_name', 'string'),
        ],
      });

      const node = new FilterInNode({});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      (node as unknown as FilterInNodeWithPrivates).autoSuggestColumns();

      expect(node.state.baseColumn).toBe('utid');
      expect(node.state.matchColumn).toBe('utid');
    });

    it('should not auto-suggest when multiple common columns', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('utid', 'int'),
        ],
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: [
          createColumnInfo('id', 'int'),
          createColumnInfo('utid', 'int'),
        ],
      });

      const node = new FilterInNode({});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      (node as unknown as FilterInNodeWithPrivates).autoSuggestColumns();

      expect(node.state.baseColumn).toBeUndefined();
      expect(node.state.matchColumn).toBeUndefined();
    });

    it('should not auto-suggest when no common columns', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [createColumnInfo('utid', 'int')],
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: [createColumnInfo('track_id', 'int')],
      });

      const node = new FilterInNode({});
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      (node as unknown as FilterInNodeWithPrivates).autoSuggestColumns();

      expect(node.state.baseColumn).toBeUndefined();
      expect(node.state.matchColumn).toBeUndefined();
    });

    it('should not overwrite existing column selection', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [
          createColumnInfo('utid', 'int'),
          createColumnInfo('name', 'string'),
        ],
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: [
          createColumnInfo('utid', 'int'),
          createColumnInfo('other', 'int'),
        ],
      });

      const node = new FilterInNode({
        baseColumn: 'name',
        matchColumn: 'other',
      });
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      (node as unknown as FilterInNodeWithPrivates).autoSuggestColumns();

      // Should not overwrite existing selections
      expect(node.state.baseColumn).toBe('name');
      expect(node.state.matchColumn).toBe('other');
    });

    it('should not suggest when no inputs connected', () => {
      const node = new FilterInNode({});

      (node as unknown as FilterInNodeWithPrivates).autoSuggestColumns();

      expect(node.state.baseColumn).toBeUndefined();
      expect(node.state.matchColumn).toBeUndefined();
    });
  });

  describe('cleanupStaleColumns', () => {
    it('should clear baseColumn when it no longer exists in primary', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [createColumnInfo('name', 'string')],
      });

      const node = new FilterInNode({baseColumn: 'utid'});
      node.primaryInput = primaryNode;

      (node as unknown as FilterInNodeWithPrivates).cleanupStaleColumns();

      expect(node.state.baseColumn).toBeUndefined();
    });

    it('should clear matchColumn when it no longer exists in match node', () => {
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: [createColumnInfo('name', 'string')],
      });

      const node = new FilterInNode({matchColumn: 'id'});
      node.secondaryInputs.connections.set(0, matchNode);

      (node as unknown as FilterInNodeWithPrivates).cleanupStaleColumns();

      expect(node.state.matchColumn).toBeUndefined();
    });

    it('should keep columns that still exist', () => {
      const primaryNode = createMockNode({
        nodeId: 'primary',
        columns: [
          createColumnInfo('utid', 'int'),
          createColumnInfo('name', 'string'),
        ],
      });
      const matchNode = createMockNode({
        nodeId: 'match',
        columns: [createColumnInfo('id', 'int')],
      });

      const node = new FilterInNode({
        baseColumn: 'utid',
        matchColumn: 'id',
      });
      node.primaryInput = primaryNode;
      node.secondaryInputs.connections.set(0, matchNode);

      (node as unknown as FilterInNodeWithPrivates).cleanupStaleColumns();

      expect(node.state.baseColumn).toBe('utid');
      expect(node.state.matchColumn).toBe('id');
    });

    it('should handle no inputs gracefully', () => {
      const node = new FilterInNode({
        baseColumn: 'utid',
        matchColumn: 'id',
      });

      (node as unknown as FilterInNodeWithPrivates).cleanupStaleColumns();

      // No primary input, so baseColumn is not checked (kept as-is)
      expect(node.state.baseColumn).toBe('utid');
      // No match node, so matchColumn is not checked (kept as-is)
      expect(node.state.matchColumn).toBe('id');
    });
  });

  describe('clone', () => {
    it('should create independent copy with same state', () => {
      const node = new FilterInNode({
        baseColumn: 'utid',
        matchColumn: 'id',
      });

      const cloned = node.clone() as FilterInNode;

      expect(cloned.type).toBe(NodeType.kFilterIn);
      expect(cloned.state.baseColumn).toBe('utid');
      expect(cloned.state.matchColumn).toBe('id');
      expect(cloned.nodeId).not.toBe(node.nodeId);
    });
  });

  describe('serializeState', () => {
    it('should serialize column selections', () => {
      const node = new FilterInNode({
        baseColumn: 'utid',
        matchColumn: 'id',
      });

      const serialized = node.serializeState() as {
        baseColumn: string;
        matchColumn: string;
      };

      expect(serialized.baseColumn).toBe('utid');
      expect(serialized.matchColumn).toBe('id');
    });

    it('should serialize secondary input node IDs', () => {
      const matchNode = createMockNode({nodeId: 'match-123'});

      const node = new FilterInNode({});
      node.secondaryInputs.connections.set(0, matchNode);

      const serialized = node.serializeState() as {
        secondaryInputNodeIds: string[];
      };

      expect(serialized.secondaryInputNodeIds).toEqual(['match-123']);
    });

    it('should serialize primaryInputId', () => {
      const primaryNode = createMockNode({nodeId: 'primary-456'});

      const node = new FilterInNode({});
      node.primaryInput = primaryNode;

      const serialized = node.serializeState() as {primaryInputId: string};

      expect(serialized.primaryInputId).toBe('primary-456');
    });
  });

  describe('deserializeState', () => {
    it('should restore column selections', () => {
      const state = FilterInNode.deserializeState({
        baseColumn: 'utid',
        matchColumn: 'id',
      });

      expect(state.baseColumn).toBe('utid');
      expect(state.matchColumn).toBe('id');
    });
  });

  describe('deserializeConnections', () => {
    it('should restore secondary input connections', () => {
      const mockNode = createMockNode({nodeId: 'match-1'});
      const nodes = new Map([['match-1', mockNode]]);

      const result = FilterInNode.deserializeConnections(nodes, {
        secondaryInputNodeIds: ['match-1'],
      });

      expect(result.secondaryInputNodes).toEqual([mockNode]);
    });

    it('should handle missing nodes gracefully', () => {
      const nodes = new Map();

      const result = FilterInNode.deserializeConnections(nodes, {
        secondaryInputNodeIds: ['nonexistent'],
      });

      expect(result.secondaryInputNodes).toEqual([]);
    });

    it('should handle empty secondaryInputNodeIds', () => {
      const nodes = new Map();

      const result = FilterInNode.deserializeConnections(nodes, {});

      expect(result.secondaryInputNodes).toEqual([]);
    });
  });

  describe('getTitle', () => {
    it('should return Filter In', () => {
      const node = new FilterInNode({});
      expect(node.getTitle()).toBe('Filter In');
    });
  });
});
