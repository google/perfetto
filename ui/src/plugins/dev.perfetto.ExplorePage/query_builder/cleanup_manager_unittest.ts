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
      dropMaterialization: jest.fn().mockResolvedValue(undefined),
      getEngine: jest.fn(),
      materializeNode: jest.fn(),
      isMaterialized: jest.fn(),
      getMaterializedTableName: jest.fn(),
      deleteNodeHash: jest.fn(),
    } as unknown as jest.Mocked<QueryExecutionService>;

    cleanupManager = new CleanupManager(mockQueryExecutionService);
  });

  function createTestNode(
    id: string,
    materialized: boolean = false,
  ): QueryNode {
    const node = new TableSourceNode({
      trace: mockTrace,
      sqlModules: mockSqlModules,
    }) as QueryNode;
    node.state.materialized = materialized;
    if (materialized) {
      node.state.materializationTableName = `_exp_materialized_${id}`;
    }
    return node;
  }

  describe('cleanupNode', () => {
    it('should call dropMaterialization for materialized node', async () => {
      const node = createTestNode('1', true);

      await cleanupManager.cleanupNode(node);

      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledWith(node);
      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledTimes(1);
      expect(mockQueryExecutionService.deleteNodeHash).toHaveBeenCalledWith(
        node,
      );
    });

    it('should not call dropMaterialization for non-materialized node', async () => {
      const node = createTestNode('1', false);

      await cleanupManager.cleanupNode(node);

      expect(
        mockQueryExecutionService.dropMaterialization,
      ).not.toHaveBeenCalled();
      expect(mockQueryExecutionService.deleteNodeHash).toHaveBeenCalledWith(
        node,
      );
    });

    it('should handle errors gracefully', async () => {
      const node = createTestNode('1', true);
      const error = new Error('Drop failed');
      mockQueryExecutionService.dropMaterialization.mockRejectedValueOnce(
        error,
      );

      // Should not throw
      await expect(cleanupManager.cleanupNode(node)).resolves.not.toThrow();

      expect(mockQueryExecutionService.dropMaterialization).toHaveBeenCalled();
    });

    it('should not call dropMaterialization when materialized is undefined', async () => {
      const node = createTestNode('1', false);
      node.state.materialized = undefined;

      await cleanupManager.cleanupNode(node);

      expect(
        mockQueryExecutionService.dropMaterialization,
      ).not.toHaveBeenCalled();
    });
  });

  describe('cleanupNodes', () => {
    it('should cleanup multiple materialized nodes in parallel', async () => {
      const node1 = createTestNode('1', true);
      const node2 = createTestNode('2', true);
      const node3 = createTestNode('3', true);

      await cleanupManager.cleanupNodes([node1, node2, node3]);

      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledTimes(3);
      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledWith(node1);
      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledWith(node2);
      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledWith(node3);
      expect(mockQueryExecutionService.deleteNodeHash).toHaveBeenCalledTimes(3);
    });

    it('should only cleanup materialized nodes', async () => {
      const node1 = createTestNode('1', true);
      const node2 = createTestNode('2', false);
      const node3 = createTestNode('3', true);

      await cleanupManager.cleanupNodes([node1, node2, node3]);

      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledTimes(2);
      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledWith(node1);
      expect(
        mockQueryExecutionService.dropMaterialization,
      ).not.toHaveBeenCalledWith(node2);
      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledWith(node3);
    });

    it('should handle empty array', async () => {
      await cleanupManager.cleanupNodes([]);

      expect(
        mockQueryExecutionService.dropMaterialization,
      ).not.toHaveBeenCalled();
    });

    it('should handle all non-materialized nodes', async () => {
      const node1 = createTestNode('1', false);
      const node2 = createTestNode('2', false);

      await cleanupManager.cleanupNodes([node1, node2]);

      expect(
        mockQueryExecutionService.dropMaterialization,
      ).not.toHaveBeenCalled();
    });

    it('should continue cleanup even if some fail', async () => {
      const node1 = createTestNode('1', true);
      const node2 = createTestNode('2', true);
      const node3 = createTestNode('3', true);

      // Make node2 cleanup fail
      mockQueryExecutionService.dropMaterialization.mockImplementation(
        (node) => {
          if (node === node2) {
            return Promise.reject(new Error('Drop failed'));
          }
          return Promise.resolve();
        },
      );

      // Should not throw
      await expect(
        cleanupManager.cleanupNodes([node1, node2, node3]),
      ).resolves.not.toThrow();

      // All three should have been attempted
      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledTimes(3);
    });

    it('should handle all cleanups failing', async () => {
      const node1 = createTestNode('1', true);
      const node2 = createTestNode('2', true);

      mockQueryExecutionService.dropMaterialization.mockRejectedValue(
        new Error('Drop failed'),
      );

      // Should not throw
      await expect(
        cleanupManager.cleanupNodes([node1, node2]),
      ).resolves.not.toThrow();

      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledTimes(2);
    });
  });

  describe('cleanupAll', () => {
    it('should cleanup all nodes', async () => {
      const node1 = createTestNode('1', true);
      const node2 = createTestNode('2', false);
      const node3 = createTestNode('3', true);

      await cleanupManager.cleanupAll([node1, node2, node3]);

      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledTimes(2);
      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledWith(node1);
      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledWith(node3);
    });

    it('should handle empty array', async () => {
      await cleanupManager.cleanupAll([]);

      expect(
        mockQueryExecutionService.dropMaterialization,
      ).not.toHaveBeenCalled();
    });

    it('should be equivalent to cleanupNodes', async () => {
      const nodes = [
        createTestNode('1', true),
        createTestNode('2', false),
        createTestNode('3', true),
      ];

      const cleanupNodesSpy = jest.spyOn(cleanupManager, 'cleanupNodes');

      await cleanupManager.cleanupAll(nodes);

      expect(cleanupNodesSpy).toHaveBeenCalledWith(nodes);
    });
  });

  describe('dispose pattern', () => {
    function createDisposableNode(
      id: string,
    ): QueryNode & {dispose: () => void} {
      const baseNode = createTestNode(id, false);
      const disposable = baseNode as QueryNode & {dispose: () => void};
      disposable.dispose = jest.fn();
      return disposable;
    }

    it('should call dispose on disposable nodes', async () => {
      const disposableNode = createDisposableNode('1');

      await cleanupManager.cleanupNode(disposableNode);

      expect(disposableNode.dispose).toHaveBeenCalledTimes(1);
    });

    it('should not throw if node is not disposable', async () => {
      const normalNode = createTestNode('1', false);

      await expect(
        cleanupManager.cleanupNode(normalNode),
      ).resolves.not.toThrow();
    });

    it('should call dispose before SQL cleanup', async () => {
      const disposableNode = createDisposableNode('1');
      disposableNode.state.materialized = true;
      disposableNode.state.materializationTableName = '_exp_materialized_1';

      const callOrder: string[] = [];
      (disposableNode.dispose as jest.Mock).mockImplementation(() => {
        callOrder.push('dispose');
      });
      mockQueryExecutionService.dropMaterialization.mockImplementation(() => {
        callOrder.push('dropMaterialization');
        return Promise.resolve();
      });

      await cleanupManager.cleanupNode(disposableNode);

      expect(callOrder).toEqual(['dispose', 'dropMaterialization']);
    });

    it('should continue cleanup even if dispose throws', async () => {
      const disposableNode = createDisposableNode('1');
      disposableNode.state.materialized = true;
      (disposableNode.dispose as jest.Mock).mockImplementation(() => {
        throw new Error('Dispose failed');
      });

      await expect(
        cleanupManager.cleanupNode(disposableNode),
      ).resolves.not.toThrow();

      expect(disposableNode.dispose).toHaveBeenCalled();
      expect(
        mockQueryExecutionService.dropMaterialization,
      ).toHaveBeenCalledWith(disposableNode);
    });

    it('should handle multiple disposable nodes', async () => {
      const node1 = createDisposableNode('1');
      const node2 = createDisposableNode('2');
      const node3 = createTestNode('3', false); // Not disposable

      await cleanupManager.cleanupNodes([node1, node2, node3]);

      expect(node1.dispose).toHaveBeenCalledTimes(1);
      expect(node2.dispose).toHaveBeenCalledTimes(1);
    });

    it('should handle dispose on node without materialization', async () => {
      const disposableNode = createDisposableNode('1');
      disposableNode.state.materialized = false;

      await cleanupManager.cleanupNode(disposableNode);

      expect(disposableNode.dispose).toHaveBeenCalledTimes(1);
      expect(
        mockQueryExecutionService.dropMaterialization,
      ).not.toHaveBeenCalled();
    });
  });
});
