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

import {uuidv4Sql} from '../../../base/uuid';
import protos from '../../../protos';
import {Engine} from '../../../trace_processor/engine';
import {Query, QueryNode} from '../query_node';

/**
 * Returns an Error if the error string is non-empty, otherwise undefined.
 */
function toError(error: string | undefined | null): Error | undefined {
  if (error !== undefined && error !== null && error !== '') {
    return new Error(error);
  }
  return undefined;
}

/**
 * Coordinates query execution for Explore Page nodes.
 *
 * TP is the source of truth for materialization state. This service handles:
 * - Execution queue to prevent race conditions
 * - Syncing all graph nodes with TP (to prevent auto-drop of disconnected graphs)
 */
export class QueryExecutionService {
  // Execution queue state
  private isExecuting = false;
  private executionQueue: Array<{
    node: QueryNode;
    operation: () => Promise<void>;
    cancelled?: boolean;
  }> = [];

  // All nodes in the graph, set at the start of processNode().
  // Used by buildAllStructuredQueries() to prevent auto-drop of disconnected graphs.
  private allNodes: QueryNode[] = [];

  // Server-generated summarizer ID, set when createSummarizer() returns.
  private summarizerId: string | undefined = undefined;

  // Staleness tracking: maps nodeId -> wasUpdated from last sync.
  // True means the node needs re-materialization (stale).
  private nodeStaleMap = new Map<string, boolean>();

  constructor(private engine: Engine) {}

  /**
   * Returns whether a node is stale (needs re-materialization) or unknown.
   *
   * A node is considered stale if:
   * - It was marked as `wasUpdated=true` in the last syncWithTP() call, OR
   * - We don't have staleness info for it yet (unknown state, treated as stale)
   *
   * A node is considered fresh (not stale) if:
   * - It was marked as `wasUpdated=false` in the last sync (already materialized,
   *   unchanged since then)
   *
   * @param nodeId The node ID to check
   * @returns true if stale or unknown, false if fresh
   */
  isNodeStale(nodeId: string): boolean {
    // Unknown nodes are treated as stale (need execution)
    if (!this.nodeStaleMap.has(nodeId)) {
      return true;
    }
    return this.nodeStaleMap.get(nodeId) ?? true;
  }

  // Ensures the summarizer is created. Returns error if creation failed.
  private async ensureSummarizerCreated(): Promise<Error | undefined> {
    if (this.summarizerId !== undefined) {
      return undefined;
    }
    const newId = `summarizer_${uuidv4Sql()}`;
    const result = await this.engine.createSummarizer(newId);
    const error = toError(result.error);
    if (error !== undefined) {
      return error;
    }
    this.summarizerId = newId;
    return undefined;
  }

  /**
   * Syncs all graph nodes with TP. Returns error if sync failed.
   *
   * Side effect: Updates `this.nodeStaleMap` with staleness info from TP.
   * After this call, `isNodeStale()` will return accurate results for all
   * nodes that were in the sync. This side effect is intentional - it's how
   * the service learns which nodes need re-materialization.
   */
  private async syncWithTP(node: QueryNode): Promise<Error | undefined> {
    // Ensure summarizer is created first
    const createError = await this.ensureSummarizerCreated();
    if (createError !== undefined) {
      return createError;
    }

    // After ensureSummarizerCreated succeeds, summarizerId is guaranteed to be set
    if (this.summarizerId === undefined) {
      return new Error('Summarizer ID not set after creation');
    }

    const structuredQueries = this.buildAllStructuredQueries();
    const spec = new protos.TraceSummarySpec();
    spec.query = structuredQueries;

    const result = await this.engine.updateSummarizerSpec(
      this.summarizerId,
      spec,
    );
    const topLevelError = toError(result.error);
    if (topLevelError !== undefined) {
      return topLevelError;
    }

    // Update staleness map from sync result.
    // wasUpdated=true means the node needs re-materialization (is stale).
    // wasUpdated=false means the node is fresh (already materialized, unchanged).
    for (const queryInfo of result.queries ?? []) {
      if (queryInfo.queryId !== undefined && queryInfo.queryId !== null) {
        this.nodeStaleMap.set(queryInfo.queryId, queryInfo.wasUpdated ?? true);
      }
    }

    // Check if this specific node had an error during sync
    const queryInfo = result.queries?.find(
      (q) => q.queryId !== undefined && q.queryId === node.nodeId,
    );
    if (queryInfo !== undefined) {
      const queryError = toError(queryInfo.error);
      if (queryError !== undefined) {
        return queryError;
      }
    }

    return undefined;
  }

  // Builds structured queries for all valid nodes in the graph.
  private buildAllStructuredQueries(): protos.PerfettoSqlStructuredQuery[] {
    const structuredQueries: protos.PerfettoSqlStructuredQuery[] = [];
    for (const node of this.allNodes) {
      if (node.validate() && node.finalCols !== undefined) {
        const sq = node.getStructuredQuery();
        if (sq !== undefined) {
          structuredQueries.push(sq);
        }
      }
    }
    return structuredQueries;
  }

  getEngine(): Engine {
    return this.engine;
  }

  // Returns undefined if query not materialized or has error.
  async getTableName(nodeId: string): Promise<string | undefined> {
    if (this.summarizerId === undefined) {
      return undefined;
    }
    const result = await this.engine.querySummarizer(this.summarizerId, nodeId);
    if (result.exists !== true || toError(result.error) !== undefined) {
      return undefined;
    }
    return result.tableName ?? undefined;
  }

  // Executes operation in FIFO order. Errors propagate to callers, queue continues.
  executeWithCoordination(
    node: QueryNode,
    operation: () => Promise<void>,
  ): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const queueItem: {
        node: QueryNode;
        operation: () => Promise<void>;
        cancelled?: boolean;
      } = {
        node,
        operation: async () => {},
      };

      const wrappedOperation = async () => {
        if (queueItem.cancelled) {
          resolve();
          return;
        }

        // Run operation and propagate result/error to caller's promise
        await operation().then(
          () => resolve(),
          (error) => reject(error),
        );
      };

      queueItem.operation = wrappedOperation;
      this.executionQueue.push(queueItem);

      if (!this.isExecuting) {
        this.processQueue();
      }
    });
  }

  private async processQueue(): Promise<void> {
    this.isExecuting = true;

    while (this.executionQueue.length > 0) {
      const current = this.executionQueue.shift()!;
      await current.operation();
    }

    this.isExecuting = false;
  }

  /**
   * Clears all pending execution requests.
   */
  clearPendingExecution(): void {
    for (const item of this.executionQueue) {
      item.cancelled = true;
    }
  }

  /**
   * Returns whether a query execution is currently in progress.
   */
  isQueryExecuting(): boolean {
    return this.isExecuting;
  }

  // Processes a node: syncs with TP, fetches result (triggering lazy materialization).
  async processNode(
    node: QueryNode,
    _engine: Engine, // Unused - using this.engine from constructor
    allNodes: QueryNode[],
    options: {
      manual: boolean;
      hasExistingResult?: boolean;
      onAnalysisStart?: () => void;
      onAnalysisComplete?: (query: Query | Error | undefined) => void;
      onExecutionStart?: () => void;
      onExecutionSuccess?: (result: {
        tableName: string;
        rowCount: number;
        columns: string[];
        durationMs: number;
      }) => void;
      onExecutionError?: (error: unknown) => void;
    },
  ): Promise<{query: Query | Error | undefined; executed: boolean}> {
    // Store allNodes for use by buildAllStructuredQueries()
    this.allNodes = allNodes;

    const autoExecute = node.state.autoExecute ?? true;

    // For autoExecute=false and not manual: sync to check staleness,
    // then show existing results if fresh, or skip if stale.
    if (!autoExecute && !options.manual) {
      let query: Query | Error | undefined;
      let executed = false;

      await this.executeWithCoordination(node, async () => {
        options.onAnalysisStart?.();

        // Sync spec with TP to get staleness info
        const syncError = await this.syncWithTP(node);
        if (syncError !== undefined) {
          query = syncError;
          options.onAnalysisComplete?.(syncError);
          return;
        }

        // Check if node is stale (needs re-materialization)
        if (this.isNodeStale(node.nodeId)) {
          // Node is stale - don't execute, UI will show "Run Query" button
          options.onAnalysisComplete?.(undefined);
          return;
        }

        // Node is fresh - get existing result from TP
        if (this.summarizerId === undefined) {
          options.onAnalysisComplete?.(undefined);
          return;
        }

        const result = await this.engine.querySummarizer(
          this.summarizerId,
          node.nodeId,
        );

        if (result.exists !== true || !result.tableName) {
          // No existing result - UI will show "Run Query" button
          options.onAnalysisComplete?.(undefined);
          return;
        }

        const resultError = toError(result.error);
        if (resultError !== undefined) {
          query = resultError;
          options.onAnalysisComplete?.(resultError);
          return;
        }

        // Construct Query object from existing result
        query = {
          sql: result.sql ?? '',
          textproto: result.textproto ?? '',
          standaloneSql: result.standaloneSql ?? '',
        };

        // Mark node as fresh (not stale) since we got existing result
        this.nodeStaleMap.set(node.nodeId, false);

        options.onAnalysisComplete?.(query);
        options.onExecutionSuccess?.({
          tableName: result.tableName,
          rowCount: Number(result.rowCount ?? 0),
          columns: result.columns ?? [],
          durationMs: result.durationMs ?? 0,
        });

        executed = true;
      });

      return {query, executed};
    }

    // Execute with coordination to prevent race conditions
    let query: Query | Error | undefined;
    let executed = false;

    await this.executeWithCoordination(node, async () => {
      options.onAnalysisStart?.();

      // Sync spec with TP (creates summarizer if needed)
      const syncError = await this.syncWithTP(node);
      if (syncError !== undefined) {
        query = syncError;
        options.onAnalysisComplete?.(syncError);
        options.onExecutionError?.(syncError);
        return;
      }

      // Query the summarizer - this triggers lazy materialization
      if (this.summarizerId === undefined) {
        const error = new Error('Summarizer ID not set');
        query = error;
        options.onAnalysisComplete?.(error);
        options.onExecutionError?.(error);
        return;
      }
      const result = await this.engine.querySummarizer(
        this.summarizerId,
        node.nodeId,
      );

      if (result.exists !== true) {
        const error = new Error(
          `Query result not found for node ${node.nodeId}`,
        );
        query = error;
        options.onAnalysisComplete?.(error);
        options.onExecutionError?.(error);
        return;
      }

      const resultError = toError(result.error);
      if (resultError !== undefined) {
        query = resultError;
        options.onAnalysisComplete?.(resultError);
        options.onExecutionError?.(resultError);
        return;
      }

      if (!result.sql || result.sql === '') {
        const error = new Error(
          `Query result missing SQL for node ${node.nodeId}`,
        );
        query = error;
        options.onAnalysisComplete?.(error);
        options.onExecutionError?.(error);
        return;
      }

      if (!result.tableName) {
        const error = new Error(
          `Query result missing table name for node ${node.nodeId}`,
        );
        query = error;
        options.onAnalysisComplete?.(error);
        options.onExecutionError?.(error);
        return;
      }

      // Construct Query object from the response (for display in Result tab)
      query = {
        sql: result.sql,
        textproto: result.textproto ?? '',
        standaloneSql: result.standaloneSql ?? '',
      };

      // Notify analysis complete
      options.onAnalysisComplete?.(query);

      // Mark node as fresh (not stale) after successful execution
      this.nodeStaleMap.set(node.nodeId, false);

      // Notify execution results
      options.onExecutionStart?.();
      options.onExecutionSuccess?.({
        tableName: result.tableName,
        rowCount: Number(result.rowCount ?? 0),
        columns: result.columns ?? [],
        durationMs: result.durationMs ?? 0,
      });

      executed = true;
    });

    return {query, executed};
  }

  // Drops all materialized tables by syncing an empty spec.
  async dropAllMaterializations(): Promise<void> {
    if (this.summarizerId === undefined) {
      return;
    }
    const emptySpec = new protos.TraceSummarySpec();
    await this.engine.updateSummarizerSpec(this.summarizerId, emptySpec);
  }
}
