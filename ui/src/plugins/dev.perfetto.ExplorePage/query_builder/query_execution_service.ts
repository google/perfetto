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

import {NUM} from '../../../trace_processor/query_result';
import {Engine} from '../../../trace_processor/engine';
import {Query, QueryNode} from '../query_node';
import {hashNodeQuery, analyzeNode, isAQuery} from './query_builder_utils';
import {getAllDownstreamNodes} from './graph_utils';

/**
 * Service for managing query execution and materialized tables for Explore Page nodes.
 *
 * # Architecture Overview
 *
 * This service centralizes all query execution concerns to solve several critical problems:
 *
 * 1. **Race Conditions**: When users rapidly interact with nodes (clicking, typing),
 *    multiple query executions can be triggered simultaneously. Without coordination,
 *    this causes:
 *    - COUNT(*) queries running against tables being dropped
 *    - Materialized tables being created/dropped in wrong order
 *    - UI state becoming inconsistent with database state
 *
 * 2. **Stale Query Execution**: During rapid user input (e.g., typing a column name),
 *    multiple analysis cycles queue operations. Without staleness detection, all
 *    queued operations execute even though only the latest is relevant.
 *
 * 3. **Performance**: Query hashing is expensive (JSON.stringify of entire node tree).
 *    Caching hashes prevents redundant computation during rapid analysis cycles.
 *
 * # Key Mechanisms
 *
 * ## FIFO Execution Queue
 * - Operations execute in order to preserve node dependencies
 * - If node A's table is needed by node B, A executes first
 * - Only one operation executes at a time (serialized execution)
 *
 * ## Staleness Detection
 * - Each queued operation stores its query hash at queue time
 * - Before execution, compares queued hash vs current hash
 * - Skips execution if query changed (operation is stale)
 *
 * ## Materialization Lifecycle
 * - Query analyzed → hash computed → operation queued
 * - Operation dequeued → staleness check → materialize query
 * - Table created → metadata fetched → results displayed
 * - On query change: drop old table → materialize new query
 *
 * # Responsibilities
 * - Query execution coordination to prevent race conditions
 * - Materialization of queries into persistent PERFETTO tables
 * - Query hash caching and invalidation for optimization
 * - Debouncing to prevent excessive operations during rapid user input
 * - Staleness detection to skip outdated operations
 */
export class QueryExecutionService {
  // Per-node debounce timers for materialization to batch rapid requests
  // Why: During typing, each keystroke triggers analysis. Debouncing prevents
  // creating/dropping tables on every keystroke, waiting for user to pause.
  // Per-node timers prevent interference between different nodes.
  private materializeTimers = new Map<string, ReturnType<typeof setTimeout>>();
  private static readonly MATERIALIZE_DEBOUNCE_MS = 300;

  // Cache of computed query hashes to avoid redundant JSON.stringify() calls
  // Key: node.nodeId, Value: JSON hash of the query
  // Why: hashNodeQuery() serializes the entire node tree to JSON, which is expensive.
  // During rapid analysis (e.g., typing), the same node may be analyzed multiple times.
  // Caching prevents redundant hashing until the query actually changes.
  private queryHashCache = new Map<string, string>();

  // Execution queue state for preventing race conditions
  // Why: Multiple async operations (materialization, dropping tables, fetching metadata)
  // can be triggered simultaneously. Without serialization, operations can interleave
  // incorrectly (e.g., DROP TABLE while COUNT(*) is running).

  // Flag: is an operation currently executing?
  // - true: queue processor is running, new operations wait in queue
  // - false: no active execution, next executeWithCoordination() starts processing
  private isExecuting: boolean = false;

  // FIFO queue of pending operations
  // Each entry contains:
  // - node: The QueryNode this operation is for
  // - operation: The async function to execute (wrapped for error/completion tracking)
  // - queryHash: The query hash at the time this operation was queued
  //   Used for staleness detection: if current hash differs, operation is skipped
  // - cancelled: Flag set by clearPendingExecution to skip this operation
  private executionQueue: Array<{
    node: QueryNode;
    operation: () => Promise<void>;
    queryHash?: string; // Hash at queue time for staleness detection
    cancelled?: boolean; // Set by clearPendingExecution
  }> = [];

  constructor(private engine: Engine) {}

  /**
   * Materializes a node's query into a table with debouncing.
   * Multiple rapid calls will be debounced to prevent excessive database operations.
   *
   * @param node The node to materialize
   * @param query The validated query to materialize
   * @param queryHash Hash of the query for change detection
   * @returns A promise that resolves to the name of the created materialized table
   */
  async materializeNode(
    node: QueryNode,
    query: Query,
    queryHash: string,
  ): Promise<string> {
    // Cancel any pending materialization for this specific node
    const existingTimer = this.materializeTimers.get(node.nodeId);
    if (existingTimer !== undefined) {
      clearTimeout(existingTimer);
    }

    // Return a promise that resolves after debouncing
    return new Promise((resolve, reject) => {
      const timer = setTimeout(async () => {
        try {
          const tableName = await this.performMaterialization(
            node,
            query,
            queryHash,
          );
          this.materializeTimers.delete(node.nodeId);
          resolve(tableName);
        } catch (error) {
          this.materializeTimers.delete(node.nodeId);
          reject(error);
        }
      }, QueryExecutionService.MATERIALIZE_DEBOUNCE_MS);
      this.materializeTimers.set(node.nodeId, timer);
    });
  }

  /**
   * Performs the actual materialization without debouncing.
   * Internal method called after debounce period.
   *
   * Note: This method can throw if the SQL query fails. The error will be
   * caught and logged by executeWithCoordination and handled by the caller.
   */
  private async performMaterialization(
    node: QueryNode,
    query: Query,
    queryHash: string,
  ): Promise<string> {
    const tableName = this.getTableName(node);

    // Build the full SQL with includes and preambles
    const includes = query.modules.map((c) => `INCLUDE PERFETTO MODULE ${c};`);
    const parts: string[] = [];
    if (includes.length > 0) {
      parts.push(includes.join('\n'));
    }
    if (query.preambles.length > 0) {
      parts.push(query.preambles.join('\n'));
    }

    // Execute the includes and preambles first
    if (parts.length > 0) {
      const fullSql = parts.join('\n');
      await this.engine.query(fullSql);
    }

    // Create or replace the materialized table
    const createTableSql = `CREATE OR REPLACE PERFETTO TABLE ${tableName} AS ${query.sql}`;
    await this.engine.query(createTableSql);

    // Update node state
    node.state.materialized = true;
    node.state.materializationTableName = tableName;
    // Store query hash for cache invalidation when query changes
    node.state.materializedQueryHash = queryHash;

    return tableName;
  }

  /**
   * Drops a materialized table for a node.
   *
   * # Critical Design Decision: State Updated BEFORE Query
   *
   * This method updates node state BEFORE executing the DROP TABLE query.
   * This ordering is critical to prevent a race condition.
   *
   * ## The Race Condition (if we updated state AFTER drop)
   *
   * Without proper ordering:
   * ```
   * async dropMaterialization(node) {
   *   const tableName = node.state.materializationTableName;
   *   await engine.query(`DROP TABLE ${tableName}`);  // ← Await here
   *   node.state.materialized = false;                  // ← State updated after
   * }
   * ```
   *
   * Scenario that breaks:
   * 1. User changes query → dropMaterialization() starts
   * 2. DROP TABLE query sent to engine (async, takes time)
   * 3. User clicks node again → runQuery() checks canReuseMaterialization()
   * 4. canReuseMaterialization() sees materialized=true → tries to COUNT(*)
   * 5. COUNT(*) query fails: "table does not exist" (being dropped!)
   * 6. DROP finishes, state updated to false (too late)
   *
   * ## The Fix (current implementation)
   *
   * Update state FIRST, then drop:
   * ```
   * async dropMaterialization(node) {
   *   const tableName = node.state.materializationTableName;
   *   node.state.materialized = false;                  // ← State updated first
   *   node.state.materializationTableName = undefined;
   *   node.state.materializedQueryHash = undefined;
   *   await engine.query(`DROP TABLE ${tableName}`);  // ← Then await
   * }
   * ```
   *
   * Now the scenario works correctly:
   * 1. User changes query → dropMaterialization() starts
   * 2. State immediately set to materialized=false
   * 3. DROP TABLE query sent to engine (async, takes time)
   * 4. User clicks node again → runQuery() checks canReuseMaterialization()
   * 5. canReuseMaterialization() sees materialized=false → creates NEW table
   * 6. DROP finishes (old table removed, new table exists)
   *
   * ## Error Handling
   *
   * If the DROP query fails:
   * - State is already cleared (materialized=false)
   * - The table is orphaned in the database (not ideal, but harmless)
   * - UI remains functional (state is consistent)
   * - Error is logged by executeWithCoordination
   * - Orphaned tables are cleaned up when trace is closed
   *
   * This is acceptable because:
   * - Failed DROPs are rare (usually mean table already gone)
   * - Orphaned tables don't break functionality
   * - Preventing the race condition is more critical than cleanup
   *
   * @param node The node whose materialized table should be dropped
   */
  async dropMaterialization(node: QueryNode): Promise<void> {
    if (!node.state.materializationTableName) {
      return;
    }

    const tableName = node.state.materializationTableName;

    // CRITICAL: Update state BEFORE awaiting the DROP query
    // This prevents race conditions where another operation checks
    // canReuseMaterialization() while the DROP is in progress
    // See detailed explanation in method documentation above
    node.state.materialized = false;
    node.state.materializationTableName = undefined;
    node.state.materializedQueryHash = undefined;

    // Execute the DROP query
    // Use query() not tryQuery() - let errors propagate to executeWithCoordination
    // If this fails, the table is orphaned but UI remains functional (state is cleared)
    await this.engine.query(`DROP TABLE IF EXISTS ${tableName}`);
  }

  /**
   * Generates a unique table name for a node's materialization.
   *
   * @param node The node to generate a table name for
   * @returns A unique table name
   */
  private getTableName(node: QueryNode): string {
    // Sanitize nodeId to prevent SQL injection and ensure valid identifier
    // Only allow alphanumeric characters and underscores
    const sanitizedId = node.nodeId.replace(/[^a-zA-Z0-9_]/g, '_');

    // Warn if sanitization changed the nodeId, as this could lead to collisions
    if (sanitizedId !== node.nodeId) {
      console.warn(
        `Node ID "${node.nodeId}" was sanitized to "${sanitizedId}" for table name.`,
      );
    }

    return `_exp_materialized_${sanitizedId}`;
  }

  /**
   * Checks if a node is currently materialized.
   *
   * @param node The node to check
   * @returns True if the node is materialized
   */
  isMaterialized(node: QueryNode): boolean {
    return node.state.materialized ?? false;
  }

  /**
   * Gets the materialized table name for a node if it exists.
   *
   * @param node The node to get the table name for
   * @returns The table name or undefined
   */
  getMaterializedTableName(node: QueryNode): string | undefined {
    return node.state.materializationTableName;
  }

  /**
   * Gets the engine instance for executing queries against materialized tables.
   *
   * This is used by SQLDataSource to query materialized tables with server-side
   * pagination, filtering, and sorting. The engine has direct access to all
   * materialized tables created by this service.
   *
   * @returns The Engine instance with access to all materialized tables
   */
  getEngine(): Engine {
    return this.engine;
  }

  /**
   * Gets the cached query hash for a node if it exists.
   *
   * @param node The node to get the cached hash for
   * @returns The cached hash or undefined if not cached
   */
  getCachedQueryHash(node: QueryNode): string | undefined {
    return this.queryHashCache.get(node.nodeId);
  }

  /**
   * Sets the cached query hash for a node.
   *
   * @param node The node to cache the hash for
   * @param hash The query hash to cache
   */
  setCachedQueryHash(node: QueryNode, hash: string): void {
    this.queryHashCache.set(node.nodeId, hash);
  }

  /**
   * Deletes the cached query hash for a node.
   * Should be called when a node is deleted to prevent memory leaks.
   *
   * This is critical because the queryHashCache Map grows unbounded as nodes
   * are created and deleted. Without cleanup, deleted nodes' hashes remain
   * in memory indefinitely.
   *
   * @param node The node to delete the hash for
   */
  deleteNodeHash(node: QueryNode): void {
    this.queryHashCache.delete(node.nodeId);
  }

  /**
   * Invalidates a node and all its downstream dependents.
   *
   * # Why This Is Needed
   *
   * When a node's operation changes (e.g., user changes a filter condition),
   * all downstream nodes that depend on this node's output need to be
   * invalidated and re-executed.
   *
   * Example node graph:
   * ```
   *   Table Source (A)
   *        ↓
   *     Filter (B) ← User changes filter condition
   *        ↓
   *   Aggregate (C)
   * ```
   *
   * When B's filter changes:
   * 1. B's query changes → needs re-materialization
   * 2. C's input changes (B's output) → C's query is different → needs re-materialization
   *
   * # What This Method Does
   *
   * For the changed node and all downstream nodes:
   * 1. **Clears cached query hash**: Forces re-computation of the query hash
   *    on next analysis. This ensures we detect that the query actually changed.
   *
   * 2. **Clears materialized query hash**: Marks the materialized table as
   *    potentially stale. The next canReuseMaterialization() check will see
   *    that materializedQueryHash is undefined and re-materialize.
   *
   * Note: We DON'T drop materialized tables here. Tables are dropped lazily
   * during the next execution when we detect the query changed. This avoids
   * unnecessary work if the user never views the downstream nodes.
   *
   * # Example Scenario
   *
   * 1. User has nodes A → B → C all materialized
   * 2. User changes B's filter
   * 3. invalidateNode(B) called:
   *    - Clears hash cache for B and C
   *    - Clears materializedQueryHash for B and C
   * 4. User clicks B:
   *    - canReuseMaterialization(B) returns false (hash undefined)
   *    - Drops old B table, creates new one
   * 5. User clicks C:
   *    - canReuseMaterialization(C) returns false (hash undefined)
   *    - Drops old C table, creates new one with updated B data
   *
   * @param node The node whose operation changed
   */
  invalidateNode(node: QueryNode): void {
    // Get all downstream nodes (including the starting node)
    // getAllDownstreamNodes() traverses the graph following output ports
    const downstreamNodes = getAllDownstreamNodes(node);

    for (const downstreamNode of downstreamNodes) {
      // Clear cached query hash to force re-computation on next analysis
      // Without this, we might reuse an old hash and incorrectly think
      // the query hasn't changed
      this.queryHashCache.delete(downstreamNode.nodeId);

      // Clear materialization state if the node was materialized
      // This marks the materialized table as potentially stale
      // Note: We keep materialized=true and tableName to avoid breaking
      // active data grids. The table is dropped lazily on next execution.
      if (downstreamNode.state.materialized) {
        downstreamNode.state.materializedQueryHash = undefined;
      }
    }
  }

  /**
   * Executes a query operation with coordination to prevent race conditions.
   *
   * # Problem This Solves
   *
   * Without coordination, rapid user interactions cause race conditions:
   *
   * Example scenario WITHOUT this method:
   * 1. User clicks node A → starts materializing table "mat_A"
   * 2. User quickly clicks node B → starts COUNT(*) on "mat_A"
   * 3. User clicks node A again → starts DROP TABLE "mat_A"
   * 4. Step 2's COUNT(*) query fails because table was dropped mid-query
   *
   * With coordination, operations are serialized:
   * 1. Operation 1 queued (materialize A)
   * 2. Operation 2 queued (count A)
   * 3. Operation 3 queued (drop A, materialize new A)
   * 4. Execute in order: 1 → 2 → 3, no race condition
   *
   * # How It Works
   *
   * This method implements a FIFO queue with two key features:
   *
   * ## 1. Serialized Execution (prevents race conditions)
   * - Only one operation executes at a time
   * - Operations execute in the order they were queued
   * - Preserves node dependencies (if node B depends on node A's table,
   *   A's materialization completes before B's query runs)
   *
   * ## 2. Staleness Detection (prevents wasted work)
   * - Each operation stores its query hash at queue time
   * - Before execution, compares queued hash vs current hash
   * - If hashes differ, query changed → skip execution
   *
   * Example of staleness detection:
   * - User types "abc" rapidly (3 keystrokes)
   * - 3 operations queued with hashes H1, H2, H3
   * - When H1 executes: current hash is H3 → skip (stale)
   * - When H2 executes: current hash is H3 → skip (stale)
   * - When H3 executes: current hash is H3 → execute (fresh)
   *
   * # Why FIFO (not "latest wins")?
   *
   * We use FIFO instead of deduplication because:
   * - Node B might depend on node A's materialized table
   * - If we cancel A's operation, B's query will fail
   * - FIFO ensures dependencies are satisfied in order
   * - Staleness detection skips outdated operations efficiently
   *
   * # Error Handling Strategy
   *
   * Errors are propagated ONLY to the caller that triggered the failed operation.
   * Other queued operations continue executing. This prevents one failed operation
   * from blocking all subsequent operations.
   *
   * Example:
   * - 3 operations queued: A, B, C
   * - B fails with SQL error
   * - A's caller receives success
   * - B's caller receives error
   * - C's caller receives success (or failure if C fails)
   *
   * # Usage
   *
   * ```typescript
   * const hash = this.getCachedQueryHash(node);
   * await queryExecutionService.executeWithCoordination(
   *   node,
   *   async () => {
   *     // Your operation here (materialize, drop table, etc.)
   *     await this.doRunQuery(node);
   *   },
   *   hash, // Optional: for staleness detection
   * );
   * ```
   *
   * @param node The QueryNode this operation is for
   * @param operation The async operation to execute (materialization, query, etc.)
   * @param queryHash Optional: query hash at queue time for staleness detection.
   *                  If provided, operation is skipped if hash changes while queued.
   * @returns Promise that resolves when this specific operation completes
   * @throws Any error thrown by the operation will be propagated to this caller only
   */
  async executeWithCoordination(
    node: QueryNode,
    operation: () => Promise<void>,
    queryHash?: string,
  ): Promise<void> {
    // These variables track the state of THIS specific operation (not all operations)
    // They're captured in closures and accessed by both the caller's promise
    // and the queue processor
    let operationError: unknown = undefined; // Error thrown by this operation
    let operationResolve: (() => void) | undefined; // Resolve function for completion
    let operationReject: ((error: unknown) => void) | undefined; // Reject function for errors
    let wasSkipped = false; // Was this operation skipped due to staleness?

    // Create queue item first so wrappedOperation can reference it
    const queueItem: {
      node: QueryNode;
      operation: () => Promise<void>;
      queryHash?: string;
      cancelled?: boolean;
    } = {
      node,
      operation: async () => {}, // Placeholder, set below
      queryHash,
    };

    // Wrap the caller's operation to add:
    // 1. Cancellation check (skip if clearPendingExecution was called)
    // 2. Staleness detection (skip if query changed)
    // 3. Error capture (catch errors for this operation)
    // 4. Completion tracking (signal when done via resolve/reject)
    const wrappedOperation = async () => {
      // CANCELLATION CHECK: Was clearPendingExecution() called?
      if (queueItem.cancelled) {
        // Operation was cancelled → skip execution but resolve promise
        wasSkipped = true;
        operationResolve?.();
        return;
      }

      // STALENESS CHECK: Did the query change while this operation waited in queue?
      // This happens during rapid user input (typing, clicking)
      if (queryHash !== undefined) {
        const currentHash = this.getCachedQueryHash(node);
        if (currentHash !== queryHash) {
          // Query changed since we queued → skip execution to save work
          // Example: User typed "abc" but we're processing "a" → skip
          wasSkipped = true;
          operationResolve?.();
          return;
        }
      }

      // Execute the actual operation (materialization, query, etc.)
      try {
        await operation();
        operationResolve?.();
      } catch (error) {
        // Capture error for THIS operation only
        // The queue processor will continue with other operations
        operationError = error;
        operationReject?.(error);
      }
    };

    // Set the operation on the queue item
    queueItem.operation = wrappedOperation;

    // Add this operation to the FIFO queue
    this.executionQueue.push(queueItem);

    // CASE 1: Another operation is currently executing
    // Wait for OUR operation to complete (it's in the queue)
    if (this.isExecuting) {
      // The queue processor (running in another async context) will execute
      // our operation eventually. Instead of polling, we wait for the operation
      // to signal completion via resolve/reject callbacks.
      await new Promise<void>((resolve, reject) => {
        operationResolve = resolve;
        operationReject = reject;
      });

      // Our operation finished. Check what happened:

      // If skipped (stale), return success (caller doesn't care about stale operations)
      if (wasSkipped) {
        return;
      }

      // If failed, error was already thrown via operationReject
      // No need to check operationError here

      // Success - our operation completed without error
      return;
    }

    // CASE 2: No operation is executing - we become the queue processor
    // Mark execution as in progress so other callers wait
    this.isExecuting = true;

    // Process ALL queued operations (including ours) in FIFO order
    // This loop processes not just our operation, but all operations that were
    // queued before and during our execution
    while (this.executionQueue.length > 0) {
      const current = this.executionQueue.shift()!;

      // Execute the operation
      // - Cancellation check happens inside wrappedOperation
      // - Staleness check happens inside wrappedOperation
      // - Errors are captured inside wrappedOperation
      // - Completion is signaled via resolve/reject callbacks
      await current.operation();

      // Note: We don't check errors here - each operation handles its own errors
      // and signals completion to its own caller via the resolve/reject callbacks
    }

    // All queued operations finished - mark execution as complete
    // Next executeWithCoordination() call will become the queue processor
    this.isExecuting = false;

    // Now check what happened to OUR specific operation
    // (we just finished processing the queue, which included our operation)

    // If our operation was skipped (stale), return success
    if (wasSkipped) {
      return;
    }

    // If our operation failed, propagate the error
    if (operationError !== undefined) {
      throw operationError;
    }

    // Our operation succeeded
  }

  /**
   * Clears all pending execution requests in the queue.
   * Called when switching nodes to avoid executing stale requests.
   *
   * # Implementation Note
   *
   * We can't simply clear the queue because operations in CASE 1
   * (line 591) are waiting for their operations to be processed.
   * If we delete their operations from the queue, their promises
   * hang forever because operationResolve never gets called.
   *
   * Instead, we mark all operations as cancelled. The queue processor
   * will continue running, but operations check the cancelled flag
   * (line 551) and skip execution while still resolving promises.
   */
  clearPendingExecution(): void {
    // Mark all queued operations as cancelled
    // Operations will skip execution but still resolve promises properly
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

  /**
   * Determines whether a query should be executed based on whether it has changed.
   *
   * # Purpose
   *
   * This method prevents duplicate executions during rapid analysis cycles.
   * When auto-execute is enabled, every analysis cycle triggers runQuery().
   * Without this check, we'd re-execute the same query multiple times unnecessarily.
   *
   * # How It Works
   *
   * Compares two hashes:
   * - **currentQueryHash**: Hash of the query from the latest analysis
   * - **materializedQueryHash**: Hash of the query that's currently materialized
   *
   * If hashes match → query unchanged → skip execution (reuse existing table)
   * If hashes differ → query changed → execute (create new table)
   *
   * # Example Scenario
   *
   * 1. User views node with query "SELECT * FROM table WHERE x > 10"
   *    - Hash: H1
   *    - shouldExecuteQuery() → true (no materializedQueryHash yet)
   *    - Materializes query, stores materializedQueryHash = H1
   *
   * 2. Analysis runs again (e.g., user hovers, triggers re-analysis)
   *    - Same query: "SELECT * FROM table WHERE x > 10"
   *    - Hash: H1 (same)
   *    - shouldExecuteQuery(H1) → false (H1 === H1)
   *    - Skips execution, reuses table
   *
   * 3. User changes filter to "x > 20"
   *    - New query: "SELECT * FROM table WHERE x > 20"
   *    - Hash: H2 (different)
   *    - shouldExecuteQuery(H2) → true (H2 !== H1)
   *    - Drops old table, materializes new query
   *
   * # Relationship to Staleness Detection
   *
   * This method is called BEFORE queuing an operation (in builder.ts).
   * Staleness detection (in executeWithCoordination) happens AFTER queuing.
   *
   * - shouldExecuteQuery(): "Should we queue this operation?"
   * - Staleness detection: "Should we execute this queued operation?"
   *
   * Both are needed:
   * - shouldExecuteQuery() prevents queuing duplicate operations
   * - Staleness detection skips outdated queued operations
   *
   * @param node The node to check
   * @param currentQueryHash The hash of the current query (from latest analysis)
   * @returns True if the query should be executed (i.e., it has changed or never executed)
   */
  shouldExecuteQuery(node: QueryNode, currentQueryHash: string): boolean {
    // If no materialized hash exists, this is the first execution
    // OR the node was invalidated (invalidateNode() clears the hash)
    if (node.state.materializedQueryHash === undefined) {
      return true;
    }

    // Execute if the query hash changed since last materialization
    // If hashes match, the existing materialized table is still valid
    return currentQueryHash !== node.state.materializedQueryHash;
  }

  /**
   * Executes a query for a node with full lifecycle management.
   *
   * This is the main entry point for query execution. It handles:
   * - Hash computation and caching
   * - Execution decision logic (should we execute or skip?)
   * - Materialization (create/reuse tables)
   * - Coordination (prevent race conditions)
   * - Metadata fetching (row count, columns)
   *
   * @param node The node to execute
   * @param query The query to execute
   * @param callbacks Callbacks for lifecycle events
   * @param callbacks.shouldAutoExecute Whether to auto-execute when query changes
   * @param callbacks.hasExistingResult Whether there's already a result displayed
   * @param callbacks.onStart Called when execution starts
   * @param callbacks.onSuccess Called when execution succeeds with results
   * @param callbacks.onError Called when execution fails with error
   */
  async executeNodeQuery(
    node: QueryNode,
    query: Query,
    callbacks: {
      shouldAutoExecute: boolean;
      hasExistingResult: boolean;
      onStart: () => void;
      onSuccess: (result: {
        tableName: string;
        rowCount: number;
        columns: string[];
        durationMs: number;
      }) => void;
      onError: (error: unknown) => void;
    },
  ): Promise<void> {
    // Compute and cache query hash
    const queryHash = this.computeQueryHash(node);
    if (queryHash === undefined) {
      callbacks.onError(
        new Error('Cannot generate query hash - invalid node structure'),
      );
      return;
    }

    // Decide if we should execute
    const canReuse = this.canReuseTable(node, queryHash);
    const queryChanged = this.shouldExecuteQuery(node, queryHash);
    const needsExecution =
      (queryChanged && callbacks.shouldAutoExecute) ||
      (canReuse && !callbacks.hasExistingResult);

    if (!needsExecution) {
      return;
    }

    // Execute with coordination
    await this.executeWithCoordination(
      node,
      async () => {
        callbacks.onStart();
        const startTime = performance.now();
        let tableName: string | undefined;
        let createdNew = false;

        try {
          // Reuse or create materialization
          if (this.canReuseTable(node, queryHash)) {
            tableName = node.state.materializationTableName!;
          } else {
            if (node.state.materialized) {
              await this.dropMaterialization(node);
            }
            tableName = await this.materializeNode(node, query, queryHash);
            createdNew = true;
          }

          // Fetch metadata
          const [countResult, schemaResult] = await Promise.all([
            this.engine.query(`SELECT COUNT(*) as count FROM ${tableName}`),
            this.engine.query(`SELECT * FROM ${tableName} LIMIT 1`),
          ]);

          callbacks.onSuccess({
            tableName,
            rowCount: Number(countResult.firstRow({count: NUM}).count),
            columns: schemaResult.columns(),
            durationMs: performance.now() - startTime,
          });
        } catch (error) {
          // Cleanup failed materialization
          if (createdNew && tableName) {
            try {
              await this.dropMaterialization(node);
            } catch (dropError) {
              console.error('Failed to cleanup materialized table:', dropError);
            }
          }
          callbacks.onError(error);
        }
      },
      queryHash,
    );
  }

  private computeQueryHash(node: QueryNode): string | undefined {
    try {
      const hash = this.hashQuery(node);
      if (hash !== undefined) {
        this.setCachedQueryHash(node, hash);
      }
      return hash;
    } catch (error) {
      console.error(
        `Failed to compute query hash for node ${node.nodeId}:`,
        error,
      );
      return undefined;
    }
  }

  private hashQuery(node: QueryNode): string | undefined {
    const result = hashNodeQuery(node);
    if (result instanceof Error) {
      console.warn(result.message);
      return undefined;
    }
    return result;
  }

  private canReuseTable(
    node: QueryNode,
    queryHash: string | undefined,
  ): boolean {
    return (
      queryHash !== undefined &&
      node.state.materialized === true &&
      node.state.materializationTableName !== undefined &&
      node.state.materializedQueryHash === queryHash
    );
  }

  /**
   * Centralized method for processing a node's query with proper autoExecute handling.
   *
   * # Purpose
   *
   * This method centralizes all autoExecute logic to avoid spreading it across
   * multiple components (NodeExplorer, Builder, DataExplorer). It handles the
   * complete flow: decide whether to analyze, analyze if needed, decide whether
   * to execute, execute if needed.
   *
   * # Behavior based on autoExecute and manual flags
   *
   * | autoExecute | manual | materialized | Behavior                              |
   * |-------------|--------|--------------|---------------------------------------|
   * | true        | false  | -            | Analyze + execute if query changed    |
   * | true        | true   | -            | Analyze + execute (forced)            |
   * | false       | false  | true         | Load existing data from table         |
   * | false       | false  | false        | Skip - show "Run Query" button        |
   * | false       | true   | -            | Analyze + execute (user clicked)      |
   *
   * When autoExecute=false:
   * - If node is already materialized: Load existing data (fast, no re-analysis)
   * - If node is not materialized: Skip everything, user must click "Run Query"
   *
   * # Usage
   *
   * ```typescript
   * // Called from NodeExplorer when node state changes
   * await service.processNode(node, engine, { manual: false, ... });
   *
   * // Called from Builder when user clicks "Run Query"
   * await service.processNode(node, engine, { manual: true, ... });
   * ```
   *
   * @param node The node to process
   * @param engine The engine for analysis and execution
   * @param options Configuration and callbacks
   * @param options.manual True when user explicitly clicked "Run Query"
   * @param options.hasExistingResult Whether there's already a result displayed
   * @param options.onAnalysisStart Called when analysis starts
   * @param options.onAnalysisComplete Called when analysis completes
   * @param options.onExecutionStart Called when execution starts
   * @param options.onExecutionSuccess Called when execution succeeds
   * @param options.onExecutionError Called when execution fails
   * @returns Object with query (if analyzed) and whether execution occurred
   */
  async processNode(
    node: QueryNode,
    engine: Engine,
    options: {
      /** True when user explicitly clicked "Run Query" */
      manual: boolean;
      /** Whether there's already a result displayed (for reuse optimization) */
      hasExistingResult?: boolean;
      /** Called when analysis starts */
      onAnalysisStart?: () => void;
      /** Called when analysis completes (with query, error, or undefined if skipped) */
      onAnalysisComplete?: (query: Query | Error | undefined) => void;
      /** Called when execution starts */
      onExecutionStart?: () => void;
      /** Called when execution succeeds */
      onExecutionSuccess?: (result: {
        tableName: string;
        rowCount: number;
        columns: string[];
        durationMs: number;
      }) => void;
      /** Called when execution fails */
      onExecutionError?: (error: unknown) => void;
    },
  ): Promise<{query: Query | Error | undefined; executed: boolean}> {
    const autoExecute = node.state.autoExecute ?? true;

    // Special case: If node is already materialized and we don't have results displayed,
    // load the existing results even when autoExecute=false.
    // This handles the case where user navigates away and back to a materialized node.
    // We only fetch metadata (count + columns) - no re-analysis or re-execution.
    if (
      !autoExecute &&
      !options.manual &&
      node.state.materialized &&
      node.state.materializationTableName &&
      !options.hasExistingResult
    ) {
      try {
        const tableName = node.state.materializationTableName;
        const startTime = performance.now();

        const [countResult, schemaResult] = await Promise.all([
          engine.query(`SELECT COUNT(*) as count FROM ${tableName}`),
          engine.query(`SELECT * FROM ${tableName} LIMIT 1`),
        ]);

        options.onExecutionSuccess?.({
          tableName,
          rowCount: Number(countResult.firstRow({count: NUM}).count),
          columns: schemaResult.columns(),
          durationMs: performance.now() - startTime,
        });

        console.debug('Analysis skipped - reusing existing materialization');
        return {query: undefined, executed: false};
      } catch {
        // If loading fails (e.g., table was dropped), clear materialization state
        // and fall through to show "Run Query" button
        node.state.materialized = false;
        node.state.materializationTableName = undefined;
        node.state.materializedQueryHash = undefined;
      }
    }

    // Skip analysis/execution if autoExecute=false and not manual
    // User must click "Run Query" to execute the query
    if (!autoExecute && !options.manual) {
      options.onAnalysisComplete?.(undefined);
      return {query: undefined, executed: false};
    }

    // Analyze the node
    options.onAnalysisStart?.();
    let query: Query | Error;
    try {
      query = await analyzeNode(node, engine);
    } catch (e) {
      query = e instanceof Error ? e : new Error(String(e));
    }
    options.onAnalysisComplete?.(query);

    // If analysis failed or returned no query, we're done
    if (!isAQuery(query)) {
      return {query, executed: false};
    }

    // Execute the query.
    // We reach here only if (autoExecute || options.manual) is true,
    // since the !autoExecute && !options.manual case returns early above.
    await this.executeNodeQuery(node, query, {
      shouldAutoExecute: true, // We've already decided to execute
      hasExistingResult: options.hasExistingResult ?? false,
      onStart: () => options.onExecutionStart?.(),
      onSuccess: (result) => options.onExecutionSuccess?.(result),
      onError: (error) => options.onExecutionError?.(error),
    });
    return {query, executed: true};
  }
}
