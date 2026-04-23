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

import {NodeType} from '../../query_node';
import {dashboardRegistry} from '../../dashboard/dashboard_registry';
import {DashboardNode} from './dashboard_node';
import {
  createMockSourceNode,
  createMockNode,
  connectNodes,
  expectValidationError,
  expectValidationSuccess,
  expectColumnNames,
  createColumnInfo,
  STANDARD_TABLE_COLUMNS,
} from '../testing/test_utils';

function makeNode(overrides: {exportName?: string} = {}): DashboardNode {
  return new DashboardNode({
    exportName: overrides.exportName,
  });
}

describe('DashboardNode', () => {
  beforeEach(() => {
    dashboardRegistry.clear();
  });

  // --- Basic properties ---

  describe('basic properties', () => {
    test('type is kDashboard', () => {
      const node = makeNode();
      expect(node.type).toBe(NodeType.kDashboard);
    });

    test('has unique nodeId', () => {
      const a = makeNode();
      const b = makeNode();
      expect(a.nodeId).not.toBe(b.nodeId);
    });

    test('starts with no primary input', () => {
      const node = makeNode();
      expect(node.primaryInput).toBeUndefined();
    });

    test('starts with empty nextNodes', () => {
      const node = makeNode();
      expect(node.nextNodes).toEqual([]);
    });
  });

  // --- Title ---

  describe('getTitle', () => {
    test('returns fixed title', () => {
      const node = makeNode();
      expect(node.getTitle()).toBe('Export to Dashboard');
    });

    test('title does not change with export name', () => {
      const node = makeNode({exportName: 'My Export'});
      expect(node.getTitle()).toBe('Export to Dashboard');
    });
  });

  // --- finalCols ---

  describe('finalCols', () => {
    test('returns empty array without input', () => {
      const node = makeNode();
      expect(node.finalCols).toEqual([]);
    });

    test('passes through input columns', () => {
      const source = createMockSourceNode();
      const node = makeNode();
      connectNodes(source, node);
      expectColumnNames(node, ['id', 'name', 'value']);
    });
  });

  // --- Validation ---

  describe('validation', () => {
    test('fails without input connected', () => {
      const node = makeNode();
      expectValidationError(node, 'No input connected');
    });

    test('succeeds with valid input', () => {
      const source = createMockSourceNode();
      const node = makeNode();
      connectNodes(source, node);
      expectValidationSuccess(node);
    });

    test('fails when input is invalid', () => {
      const invalidSource = createMockNode({
        validate: () => false,
        getTitle: () => 'Bad Source',
      });
      const node = makeNode();
      connectNodes(invalidSource, node);
      expectValidationError(node, "Input 'Bad Source' is invalid");
    });

    test('clears previous issues before validating', () => {
      const node = makeNode();
      // First validation: fails.
      node.validate();
      expect(node.state.issues?.queryError).toBeTruthy();

      // Connect valid input, re-validate: succeeds.
      const source = createMockSourceNode();
      connectNodes(source, node);
      expectValidationSuccess(node);
    });

    test('lazily creates issues object on first error', () => {
      const node = makeNode();
      expect(node.state.issues).toBeUndefined();
      node.validate();
      expect(node.state.issues).toBeDefined();
    });
  });

  // --- Structured query ---

  describe('getStructuredQuery', () => {
    test('returns undefined without input', () => {
      const node = makeNode();
      expect(node.getStructuredQuery()).toBeUndefined();
    });

    test('passes through input structured query', () => {
      const sq = {id: 'test-sq'};
      const source = createMockNode({
        getStructuredQuery: () =>
          sq as ReturnType<typeof source.getStructuredQuery>,
      });
      const node = makeNode();
      connectNodes(source, node);
      expect(node.getStructuredQuery()).toBe(sq);
    });
  });

  // --- Clone ---

  describe('clone', () => {
    test('creates a new node with same export name', () => {
      const node = makeNode({exportName: 'My Export'});
      const cloned = node.clone() as DashboardNode;
      expect(cloned.nodeId).not.toBe(node.nodeId);
      expect(cloned.state.exportName).toBe('My Export');
    });

    test('cloned node has no primary input', () => {
      const source = createMockSourceNode();
      const node = makeNode();
      connectNodes(source, node);
      const cloned = node.clone();
      expect(cloned.primaryInput).toBeUndefined();
    });
  });

  // --- Serialization ---

  describe('serialization', () => {
    test('serializes exportName', () => {
      const node = makeNode({exportName: 'My Data'});
      const serialized = node.serializeState();
      expect(serialized.exportName).toBe('My Data');
    });

    test('serializes undefined exportName', () => {
      const node = makeNode();
      const serialized = node.serializeState();
      expect(serialized.exportName).toBeUndefined();
    });

    test('deserializeState restores exportName', () => {
      const state = DashboardNode.deserializeState({exportName: 'Restored'});
      expect(state.exportName).toBe('Restored');
    });

    test('deserializeState handles missing exportName', () => {
      const state = DashboardNode.deserializeState({});
      expect(state.exportName).toBeUndefined();
    });

    test('serialize then deserialize round-trip preserves exportName', () => {
      const node = makeNode({exportName: 'Round Trip'});
      const serialized = node.serializeState();
      const restored = DashboardNode.deserializeState(serialized);
      expect(restored.exportName).toBe('Round Trip');
    });
  });

  // --- Publishing to registry ---

  describe('publishExportedSource (via onPrevNodesUpdated)', () => {
    test('publishes source to registry when input connected', () => {
      const source = createMockSourceNode('src-1');
      const node = makeNode({exportName: 'Test Export'});
      connectNodes(source, node);
      node.onPrevNodesUpdated();

      const exported = dashboardRegistry.getExportedSource(node.nodeId);
      expect(exported).toBeDefined();
      expect(exported?.name).toBe('Test Export');
      expect(exported?.nodeId).toBe(node.nodeId);
    });

    test('published source has correct columns', () => {
      const source = createMockSourceNode();
      const node = makeNode();
      connectNodes(source, node);
      node.onPrevNodesUpdated();

      const exported = dashboardRegistry.getExportedSource(node.nodeId);
      expect(exported?.columns).toEqual([
        {name: 'id', type: {kind: 'int'}},
        {name: 'name', type: {kind: 'string'}},
        {name: 'value', type: {kind: 'int'}},
      ]);
    });

    test('uses input title as name when no exportName', () => {
      const source = createMockNode({
        getTitle: () => 'Slices Table',
        columns: STANDARD_TABLE_COLUMNS(),
      });
      const node = makeNode();
      connectNodes(source, node);
      node.onPrevNodesUpdated();

      const exported = dashboardRegistry.getExportedSource(node.nodeId);
      expect(exported?.name).toBe('Slices Table');
    });

    test('removes exported source when input disconnected', () => {
      const source = createMockSourceNode();
      const node = makeNode();
      connectNodes(source, node);
      node.onPrevNodesUpdated();
      expect(dashboardRegistry.getExportedSource(node.nodeId)).toBeDefined();

      // Disconnect input.
      node.primaryInput = undefined;
      node.onPrevNodesUpdated();
      expect(dashboardRegistry.getExportedSource(node.nodeId)).toBeUndefined();
    });

    test('updates exported source when columns change', () => {
      const source = createMockNode({
        columns: [createColumnInfo('a', 'int')],
      });
      const node = makeNode();
      connectNodes(source, node);
      node.onPrevNodesUpdated();
      expect(dashboardRegistry.getExportedSource(node.nodeId)?.columns).toEqual(
        [{name: 'a', type: {kind: 'int'}}],
      );

      // Change source columns.
      (source as {finalCols: typeof source.finalCols}).finalCols = [
        createColumnInfo('b', 'string'),
        createColumnInfo('c', 'double'),
      ];
      node.onPrevNodesUpdated();
      expect(dashboardRegistry.getExportedSource(node.nodeId)?.columns).toEqual(
        [
          {name: 'b', type: {kind: 'string'}},
          {name: 'c', type: {kind: 'double'}},
        ],
      );
    });

    test('multiple onPrevNodesUpdated calls overwrite (not duplicate)', () => {
      const source = createMockSourceNode();
      const node = makeNode({exportName: 'First'});
      connectNodes(source, node);
      node.onPrevNodesUpdated();
      node.onPrevNodesUpdated();
      node.onPrevNodesUpdated();

      // Still exactly one exported source for this node.
      const all = dashboardRegistry.getAllExportedSources();
      const matching = all.filter((s) => s.nodeId === node.nodeId);
      expect(matching).toHaveLength(1);
    });

    test('resolves tableName via requestExecution', async () => {
      const mockGetTable = jest.fn().mockResolvedValue('table_42');
      const mockRequest = jest.fn().mockResolvedValue(undefined);
      const source = createMockSourceNode('src-1');
      const node = new DashboardNode({
        getTableNameForNode: mockGetTable,
        requestNodeExecution: mockRequest,
      });
      connectNodes(source, node);
      node.onPrevNodesUpdated();

      // Table name is not eagerly resolved on publish.
      expect(mockGetTable).not.toHaveBeenCalled();

      // Calling requestExecution triggers both execution and table resolution.
      const exported = dashboardRegistry.getExportedSource(node.nodeId);
      await exported?.requestExecution?.();
      expect(mockRequest).toHaveBeenCalledWith('src-1');
      expect(mockGetTable).toHaveBeenCalledWith('src-1');
      expect(exported?.tableName).toBe('table_42');
    });

    test('published source includes requestExecution callback', () => {
      const mockRequest = jest.fn().mockResolvedValue(undefined);
      const source = createMockSourceNode('src-1');
      const node = new DashboardNode({
        requestNodeExecution: mockRequest,
      });
      connectNodes(source, node);
      node.onPrevNodesUpdated();

      const exported = dashboardRegistry.getExportedSource(node.nodeId);
      expect(exported?.requestExecution).toBeDefined();
      exported?.requestExecution?.();
      expect(mockRequest).toHaveBeenCalledWith('src-1');
    });
  });

  // --- Export name resolution ---

  describe('export name resolution', () => {
    test('uses exportName when set', () => {
      const source = createMockNode({getTitle: () => 'Source'});
      const node = makeNode({exportName: 'Custom Name'});
      connectNodes(source, node);
      node.onPrevNodesUpdated();
      expect(dashboardRegistry.getExportedSource(node.nodeId)?.name).toBe(
        'Custom Name',
      );
    });

    test('falls back to input title when exportName is empty', () => {
      const source = createMockNode({getTitle: () => 'My Table'});
      const node = makeNode({exportName: ''});
      connectNodes(source, node);
      node.onPrevNodesUpdated();
      expect(dashboardRegistry.getExportedSource(node.nodeId)?.name).toBe(
        'My Table',
      );
    });

    test('falls back to input title when exportName is whitespace', () => {
      const source = createMockNode({getTitle: () => 'My Table'});
      const node = makeNode({exportName: '   '});
      connectNodes(source, node);
      node.onPrevNodesUpdated();
      expect(dashboardRegistry.getExportedSource(node.nodeId)?.name).toBe(
        'My Table',
      );
    });

    test('uses "Unnamed export" when no exportName and no input', () => {
      const node = makeNode();
      const details = node.nodeDetails();
      // Verify the rendered vnode tree contains the fallback text.
      const rendered = JSON.stringify(details.content);
      expect(rendered).toContain('Unnamed export');
    });
  });
});
