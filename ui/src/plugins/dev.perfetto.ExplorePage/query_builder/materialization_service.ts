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

import {Engine} from '../../../trace_processor/engine';
import {Query, QueryNode} from '../query_node';

/**
 * Service for managing materialized tables for Explore Page nodes.
 * Materialization creates persistent tables using CREATE OR REPLACE PERFETTO TABLE.
 *
 * Includes debouncing to prevent excessive materialization calls during rapid
 * user input (e.g., typing column names).
 */
export class MaterializationService {
  private materializeTimer?: ReturnType<typeof setTimeout>;
  private static readonly MATERIALIZE_DEBOUNCE_MS = 300;

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
    // Cancel any pending materialization
    if (this.materializeTimer !== undefined) {
      clearTimeout(this.materializeTimer);
    }

    // Return a promise that resolves after debouncing
    return new Promise((resolve, reject) => {
      this.materializeTimer = setTimeout(async () => {
        try {
          const tableName = await this.performMaterialization(
            node,
            query,
            queryHash,
          );
          this.materializeTimer = undefined;
          resolve(tableName);
        } catch (error) {
          this.materializeTimer = undefined;
          reject(error);
        }
      }, MaterializationService.MATERIALIZE_DEBOUNCE_MS);
    });
  }

  /**
   * Performs the actual materialization without debouncing.
   * Internal method called after debounce period.
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
   * @param node The node whose materialized table should be dropped
   */
  async dropMaterialization(node: QueryNode): Promise<void> {
    if (!node.state.materializationTableName) {
      return;
    }

    const tableName = node.state.materializationTableName;
    // Use query() not tryQuery() - we want to know if drop fails
    await this.engine.query(`DROP TABLE IF EXISTS ${tableName}`);

    // Only update state if drop succeeded
    node.state.materialized = false;
    node.state.materializationTableName = undefined;
    node.state.materializedQueryHash = undefined;
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
}
