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
    const node = new TableSourceNode(
      {},
      {trace: mockTrace, sqlModules: mockSqlModules},
    ) as QueryNode;
    // Override nodeId for testing
    Object.defineProperty(node, 'nodeId', {value: id, writable: true});
    return node;
  }

  // Mock engine to simulate a successful execution of a given node.
  // Sets wasUpdated=true (stale after sync) but querySummarizer returns a valid
  // result, so the full execution path succeeds.
  function mockSuccessfulExecution(nodeId: string) {
    mockEngine.createSummarizer = jest.fn().mockResolvedValue({error: ''});
    mockEngine.updateSummarizerSpec = jest.fn().mockResolvedValue({
      error: '',
      queries: [{queryId: nodeId, wasUpdated: true, error: ''}],
    });
    mockEngine.querySummarizer = jest.fn().mockResolvedValue({
      exists: true,
      tableName: 'test_table',
      sql: 'SELECT 1',
      textproto: '',
      standaloneSql: 'SELECT 1',
      rowCount: 1,
      columns: ['col1'],
      durationMs: 10,
      error: '',
    });
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
    it('should propagate errors from operations', async () => {
      const node = createTestNode('node1');

      const throwingOp = async () => {
        throw new Error('Test error');
      };

      // Errors propagate - operations must handle their own errors
      await expect(
        service.executeWithCoordination(node, throwingOp),
      ).rejects.toThrow('Test error');
    });

    it('should continue processing queue after error', async () => {
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
        service.executeWithCoordination(node2, op2).catch(() => {}), // Caller handles error
        service.executeWithCoordination(node3, op3),
      ];

      await Promise.all(promises);

      // All three should execute - queue continues after error
      expect(executionOrder).toEqual(['op1', 'op2', 'op3']);
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

      // Brief wait to let queue processing complete (sets isExecuting = false)
      await new Promise((resolve) => setTimeout(resolve, 0));

      expect(isExecutingDuringOp).toBe(true);
      expect(service.isQueryExecuting()).toBe(false);
    });
  });

  describe('handleManualNodeQueryChange', () => {
    it('returns false and marks stale for a node with no execution history', () => {
      const result = service.handleManualNodeQueryChange('unknown-node');
      expect(result).toBe(false);
      expect(service.isNodeStale('unknown-node')).toBe(true);
    });

    it('returns true (column discovery) for first SQ change after successful execution', async () => {
      const node = createTestNode('node1');
      mockSuccessfulExecution('node1');
      await service.processNode(node, mockEngine, [node], {manual: true});

      expect(service.handleManualNodeQueryChange('node1')).toBe(true);
    });

    it('node stays fresh after column discovery (returning true)', async () => {
      const node = createTestNode('node1');
      mockSuccessfulExecution('node1');
      await service.processNode(node, mockEngine, [node], {manual: true});

      service.handleManualNodeQueryChange('node1');

      // Column discovery should not have marked the node stale
      expect(service.isNodeStale('node1')).toBe(false);
    });

    it('returns false for the second SQ change after execution — only one column discovery slot per execution (risk #1)', async () => {
      const node = createTestNode('node1');
      mockSuccessfulExecution('node1');
      await service.processNode(node, mockEngine, [node], {manual: true});

      // First change: column discovery (slot consumed)
      service.handleManualNodeQueryChange('node1');

      // Second change: treated as a real user edit
      const result = service.handleManualNodeQueryChange('node1');
      expect(result).toBe(false);
      expect(service.isNodeStale('node1')).toBe(true);
    });

    it('each execution grants exactly one column discovery slot', async () => {
      const node = createTestNode('node1');
      mockSuccessfulExecution('node1');

      // First execution
      await service.processNode(node, mockEngine, [node], {manual: true});
      expect(service.handleManualNodeQueryChange('node1')).toBe(true); // discovery
      expect(service.handleManualNodeQueryChange('node1')).toBe(false); // user edit

      // Second execution re-grants the slot
      await service.processNode(node, mockEngine, [node], {manual: true});
      expect(service.handleManualNodeQueryChange('node1')).toBe(true); // discovery again
      expect(service.handleManualNodeQueryChange('node1')).toBe(false); // user edit again
    });

    it('tracks column discovery slots independently per node', async () => {
      const node1 = createTestNode('node1');
      const node2 = createTestNode('node2');
      mockSuccessfulExecution('node1');

      // Only node1 is executed
      await service.processNode(node1, mockEngine, [node1, node2], {
        manual: true,
      });

      // node1 gets a column discovery slot; node2 does not
      expect(service.handleManualNodeQueryChange('node1')).toBe(true);
      expect(service.handleManualNodeQueryChange('node2')).toBe(false);
    });
  });

  describe('initializedNodes / skip path', () => {
    // Stale mock: node needs re-materialization (wasUpdated=true, exists=false).
    function mockInitialLoad(nodeId: string) {
      mockEngine.createSummarizer = jest.fn().mockResolvedValue({error: ''});
      mockEngine.updateSummarizerSpec = jest.fn().mockResolvedValue({
        error: '',
        queries: [{queryId: nodeId, wasUpdated: true, error: ''}],
      });
      mockEngine.querySummarizer = jest.fn().mockResolvedValue({
        exists: false,
      });
    }

    // Fresh mock: node is already materialized (wasUpdated=false, exists=true).
    // This populates justExecutedNodes after the initial load, so the next
    // !manual call is correctly classified as column discovery (not user edit).
    function mockFreshNode(nodeId: string) {
      mockEngine.createSummarizer = jest.fn().mockResolvedValue({error: ''});
      mockEngine.updateSummarizerSpec = jest.fn().mockResolvedValue({
        error: '',
        queries: [{queryId: nodeId, wasUpdated: false, error: ''}],
      });
      mockEngine.querySummarizer = jest.fn().mockResolvedValue({
        exists: true,
        tableName: 'test_table',
        sql: 'SELECT 1',
        textproto: '',
        standaloneSql: 'SELECT 1',
        rowCount: 1,
        columns: ['col1'],
        durationMs: 10,
        error: '',
      });
    }

    it('fires onAnalysisStart on initial load and on user edits, but NOT on column discovery', async () => {
      const node = createTestNode('node1');
      node.context.autoExecute = false;
      mockInitialLoad('node1');

      const analysisStartCalls: number[] = [];

      // First call: initial load — should fire onAnalysisStart
      await service.processNode(node, mockEngine, [node], {
        manual: false,
        onAnalysisStart: () => analysisStartCalls.push(1),
        onAnalysisComplete: () => {},
      });
      expect(analysisStartCalls).toHaveLength(1);

      // Second call: skip path user edit (node stale, not in justExecutedNodes)
      // — onAnalysisStart fires so the caller can clear its query state
      await service.processNode(node, mockEngine, [node], {
        manual: false,
        onAnalysisStart: () => analysisStartCalls.push(2),
        onAnalysisComplete: () => {},
      });
      expect(analysisStartCalls).toHaveLength(2);
    });

    it('fires onAnalysisComplete(undefined) on user edit in skip path', async () => {
      const node = createTestNode('node1');
      node.context.autoExecute = false;
      mockInitialLoad('node1');

      // Initialize the node
      await service.processNode(node, mockEngine, [node], {
        manual: false,
        onAnalysisComplete: () => {},
      });

      const completeCalls: Array<unknown> = [];

      // User edit — should clear query
      await service.processNode(node, mockEngine, [node], {
        manual: false,
        onAnalysisComplete: (q) => completeCalls.push(q),
      });

      expect(completeCalls).toEqual([undefined]);
    });

    it('fires no callbacks on column discovery in skip path', async () => {
      const node = createTestNode('node1');
      node.context.autoExecute = false;
      // Successful manual execution populates justExecutedNodes and initializedNodes
      mockSuccessfulExecution('node1');

      await service.processNode(node, mockEngine, [node], {manual: true});

      const analysisStartCalls: number[] = [];
      const completeCalls: unknown[] = [];

      // Next !manual call: column discovery — no callbacks should fire
      await service.processNode(node, mockEngine, [node], {
        manual: false,
        onAnalysisStart: () => analysisStartCalls.push(1),
        onAnalysisComplete: (q) => completeCalls.push(q),
      });

      expect(analysisStartCalls).toHaveLength(0);
      expect(completeCalls).toHaveLength(0);
    });

    it('resetNode forces the next processNode call to re-run initial load', async () => {
      const node = createTestNode('node1');
      node.context.autoExecute = false;
      // Fresh mock: initial load retrieves existing result, populating
      // justExecutedNodes so the second call is column discovery (no callbacks).
      mockFreshNode('node1');

      const analysisStartCalls: number[] = [];

      // Initial load: fires onAnalysisStart, retrieves existing result
      await service.processNode(node, mockEngine, [node], {
        manual: false,
        onAnalysisStart: () => analysisStartCalls.push(1),
        onAnalysisComplete: () => {},
      });
      expect(analysisStartCalls).toHaveLength(1);

      // Skip path — column discovery (justExecutedNodes has node1) — no callbacks
      await service.processNode(node, mockEngine, [node], {
        manual: false,
        onAnalysisStart: () => analysisStartCalls.push(2),
        onAnalysisComplete: () => {},
      });
      expect(analysisStartCalls).toHaveLength(1);

      // resetNode clears initialized state (queued, so runs before next processNode)
      service.resetNode('node1', node);

      // Next call re-runs initial load — onAnalysisStart fires again
      await service.processNode(node, mockEngine, [node], {
        manual: false,
        onAnalysisStart: () => analysisStartCalls.push(3),
        onAnalysisComplete: () => {},
      });
      expect(analysisStartCalls).toHaveLength(2);
    });
  });
});
