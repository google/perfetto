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

import {QueryExecutionService} from './query_execution_service';
import {QueryNode} from '../query_node';
import {TableSourceNode} from './nodes/sources/table_source';
import {Trace} from '../../../public/trace';
import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {Engine} from '../../../trace_processor/engine';

describe('QueryExecutionService', () => {
  let mockEngine: jest.Mocked<Engine>;
  let service: QueryExecutionService;
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

    // Create a mock Engine
    mockEngine = {
      query: jest.fn().mockResolvedValue({
        firstRow: () => ({count: 0}),
        columns: () => [],
      }),
    } as unknown as jest.Mocked<Engine>;

    service = new QueryExecutionService(mockEngine);
  });

  function createTestNode(id: string): QueryNode {
    const node = new TableSourceNode({
      trace: mockTrace,
      sqlModules: mockSqlModules,
    }) as QueryNode;
    // Override nodeId for testing
    Object.defineProperty(node, 'nodeId', {value: id, writable: true});
    return node;
  }

  describe('executeWithCoordination - FIFO Queue', () => {
    it('should execute operations in FIFO order', async () => {
      const executionOrder: string[] = [];
      const node1 = createTestNode('node1');
      const node2 = createTestNode('node2');
      const node3 = createTestNode('node3');

      const op1 = async () => {
        await new Promise((resolve) => setTimeout(resolve, 10));
        executionOrder.push('op1');
      };
      const op2 = async () => {
        executionOrder.push('op2');
      };
      const op3 = async () => {
        executionOrder.push('op3');
      };

      // Trigger all three rapidly
      const promises = [
        service.executeWithCoordination(node1, op1),
        service.executeWithCoordination(node2, op2),
        service.executeWithCoordination(node3, op3),
      ];

      await Promise.all(promises);

      // Should execute in FIFO order
      expect(executionOrder).toEqual(['op1', 'op2', 'op3']);
    });

    it('should process entire queue before stopping', async () => {
      const executionOrder: string[] = [];
      const node = createTestNode('node1');

      // Queue 5 operations
      const promises = [];
      for (let i = 1; i <= 5; i++) {
        const op = async () => {
          executionOrder.push(`op${i}`);
        };
        promises.push(service.executeWithCoordination(node, op));
      }

      await Promise.all(promises);

      expect(executionOrder).toEqual(['op1', 'op2', 'op3', 'op4', 'op5']);
    });

    it('should not execute operations concurrently', async () => {
      let concurrentExecutions = 0;
      let maxConcurrent = 0;
      const node = createTestNode('node1');

      const createOperation = () => async () => {
        concurrentExecutions++;
        maxConcurrent = Math.max(maxConcurrent, concurrentExecutions);
        await new Promise((resolve) => setTimeout(resolve, 10));
        concurrentExecutions--;
      };

      const promises = [
        service.executeWithCoordination(node, createOperation()),
        service.executeWithCoordination(node, createOperation()),
        service.executeWithCoordination(node, createOperation()),
      ];

      await Promise.all(promises);

      // Should never have more than 1 concurrent execution
      expect(maxConcurrent).toBe(1);
    });
  });

  describe('executeWithCoordination - Error Handling', () => {
    it('should propagate errors to caller but continue processing queue', async () => {
      const executionOrder: string[] = [];
      const node1 = createTestNode('node1');
      const node2 = createTestNode('node2');
      const node3 = createTestNode('node3');

      const op1 = async () => {
        executionOrder.push('op1');
      };
      const op2 = async () => {
        executionOrder.push('op2');
        throw new Error('Intentional error in op2');
      };
      const op3 = async () => {
        executionOrder.push('op3');
      };

      const promises = [
        service.executeWithCoordination(node1, op1),
        service.executeWithCoordination(node2, op2).catch((e) => e),
        service.executeWithCoordination(node3, op3),
      ];

      await Promise.all(promises);

      // All three should execute despite error in op2
      expect(executionOrder).toEqual(['op1', 'op2', 'op3']);
    });

    it('should propagate errors to caller', async () => {
      const node = createTestNode('node1');

      const throwingOp = async () => {
        throw new Error('Test error');
      };

      // Should throw
      await expect(
        service.executeWithCoordination(node, throwingOp),
      ).rejects.toThrow('Test error');
    });
  });

  describe('clearPendingExecution', () => {
    it('should clear all pending operations', async () => {
      const executionOrder: string[] = [];
      const node = createTestNode('node1');

      // Start a long-running operation
      const op1 = async () => {
        await new Promise((resolve) => setTimeout(resolve, 50));
        executionOrder.push('op1');
      };

      // Queue it
      const promise1 = service.executeWithCoordination(node, op1);

      // Queue more operations while op1 is running
      await new Promise((resolve) => setTimeout(resolve, 5));
      const op2 = async () => {
        executionOrder.push('op2');
      };
      const op3 = async () => {
        executionOrder.push('op3');
      };
      service.executeWithCoordination(node, op2);
      service.executeWithCoordination(node, op3);

      // Clear the pending queue (op2 and op3 should not execute)
      service.clearPendingExecution();

      // Wait for op1 to finish
      await promise1;

      // Only op1 should have executed
      expect(executionOrder).toEqual(['op1']);
    });
  });

  describe('isQueryExecuting', () => {
    it('should return true while executing', async () => {
      const node = createTestNode('node1');
      let isExecutingDuringOp = false;

      const op = async () => {
        isExecutingDuringOp = service.isQueryExecuting();
        await new Promise((resolve) => setTimeout(resolve, 10));
      };

      expect(service.isQueryExecuting()).toBe(false);

      const promise = service.executeWithCoordination(node, op);

      // Brief wait to ensure operation has started
      await new Promise((resolve) => setTimeout(resolve, 5));

      await promise;

      expect(isExecutingDuringOp).toBe(true);
      expect(service.isQueryExecuting()).toBe(false);
    });
  });

  describe('shouldExecuteQuery', () => {
    it('should return true for first execution', () => {
      const node = createTestNode('node1');
      const hash = 'hash123';

      expect(service.shouldExecuteQuery(node, hash)).toBe(true);
    });

    it('should return false if hash matches materialized hash', () => {
      const node = createTestNode('node1');
      const hash = 'hash123';

      node.state.materializedQueryHash = hash;

      expect(service.shouldExecuteQuery(node, hash)).toBe(false);
    });

    it('should return true if hash changed', () => {
      const node = createTestNode('node1');
      const oldHash = 'hash123';
      const newHash = 'hash456';

      node.state.materializedQueryHash = oldHash;

      expect(service.shouldExecuteQuery(node, newHash)).toBe(true);
    });
  });

  describe('getCachedQueryHash and setCachedQueryHash', () => {
    it('should cache and retrieve query hashes', () => {
      const node = createTestNode('node1');
      const hash = 'hash123';

      expect(service.getCachedQueryHash(node)).toBeUndefined();

      service.setCachedQueryHash(node, hash);

      expect(service.getCachedQueryHash(node)).toBe(hash);
    });
  });
});
