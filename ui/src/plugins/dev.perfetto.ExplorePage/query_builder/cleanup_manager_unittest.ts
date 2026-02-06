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

import {CleanupManager} from './cleanup_manager';
import {QueryExecutionService} from './query_execution_service';
import {QueryNode} from '../query_node';
import {TableSourceNode} from './nodes/sources/table_source';
import {Trace} from '../../../public/trace';
import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';

describe('CleanupManager', () => {
  let mockQueryExecutionService: jest.Mocked<QueryExecutionService>;
  let cleanupManager: CleanupManager;
  let mockTrace: Trace;
  let mockSqlModules: SqlModules;

  beforeEach(() => {
    mockTrace = {
      traceInfo: {
        traceTitle: 'test_trace',
      },
    } as Trace;

    mockSqlModules = {
      listTables: () => [],
      getTable: () => null,
      listModules: () => [],
      listTablesNames: () => [],
      getModuleForTable: () => undefined,
    } as unknown as SqlModules;

    // Create a mock QueryExecutionService
    mockQueryExecutionService = {
      getEngine: jest.fn(),
      materializeNode: jest.fn(),
      dropAllMaterializations: jest.fn().mockResolvedValue(undefined),
    } as unknown as jest.Mocked<QueryExecutionService>;

    cleanupManager = new CleanupManager(mockQueryExecutionService);
  });

  function createTestNode(id: string): QueryNode {
    const node = new TableSourceNode({
      trace: mockTrace,
      sqlModules: mockSqlModules,
    }) as QueryNode;
    // Use Object.defineProperty to set nodeId since it's readonly
    Object.defineProperty(node, 'nodeId', {value: id, writable: false});
    return node;
  }

  describe('cleanupNode', () => {
    it('should handle errors gracefully in dispose', () => {
      const node = createTestNode('1') as QueryNode & {dispose: () => void};
      node.dispose = jest.fn().mockImplementation(() => {
        throw new Error('Dispose failed');
      });

      // Should not throw
      expect(() => cleanupManager.cleanupNode(node)).not.toThrow();
    });
  });

  describe('cleanupNodes', () => {
    it('should cleanup multiple nodes', () => {
      const node1 = createTestNode('1');
      const node2 = createTestNode('2');
      const node3 = createTestNode('3');

      // Should not throw
      expect(() =>
        cleanupManager.cleanupNodes([node1, node2, node3]),
      ).not.toThrow();
    });

    it('should handle empty array', () => {
      expect(() => cleanupManager.cleanupNodes([])).not.toThrow();
    });
  });

  describe('cleanupAll', () => {
    it('should cleanup all nodes and drop all materializations', async () => {
      const node1 = createTestNode('1');
      const node2 = createTestNode('2');
      const node3 = createTestNode('3');

      await cleanupManager.cleanupAll([node1, node2, node3]);

      expect(
        mockQueryExecutionService.dropAllMaterializations,
      ).toHaveBeenCalledTimes(1);
    });

    it('should handle empty array', async () => {
      await cleanupManager.cleanupAll([]);

      expect(
        mockQueryExecutionService.dropAllMaterializations,
      ).toHaveBeenCalledTimes(1);
    });
  });

  describe('dispose pattern', () => {
    function createDisposableNode(
      id: string,
    ): QueryNode & {dispose: () => void} {
      const baseNode = createTestNode(id);
      const disposable = baseNode as QueryNode & {dispose: () => void};
      disposable.dispose = jest.fn();
      return disposable;
    }

    it('should call dispose on disposable nodes', () => {
      const disposableNode = createDisposableNode('1');

      cleanupManager.cleanupNode(disposableNode);

      expect(disposableNode.dispose).toHaveBeenCalledTimes(1);
    });

    it('should not throw if node is not disposable', () => {
      const normalNode = createTestNode('1');

      expect(() => cleanupManager.cleanupNode(normalNode)).not.toThrow();
    });

    it('should continue cleanup even if dispose throws', () => {
      const disposableNode = createDisposableNode('1');
      (disposableNode.dispose as jest.Mock).mockImplementation(() => {
        throw new Error('Dispose failed');
      });

      expect(() => cleanupManager.cleanupNode(disposableNode)).not.toThrow();

      expect(disposableNode.dispose).toHaveBeenCalled();
    });

    it('should handle multiple disposable nodes', () => {
      const node1 = createDisposableNode('1');
      const node2 = createDisposableNode('2');
      const node3 = createTestNode('3'); // Not disposable

      cleanupManager.cleanupNodes([node1, node2, node3]);

      expect(node1.dispose).toHaveBeenCalledTimes(1);
      expect(node2.dispose).toHaveBeenCalledTimes(1);
    });
  });
});
