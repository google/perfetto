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

import {QueryNode} from '../query_node';
import {QueryExecutionService} from './query_execution_service';

/**
 * Centralized manager for resource cleanup in the Explore Page.
 *
 * Responsibilities:
 * - Cleans up JavaScript resources (intervals, subscriptions) via dispose()
 * - Cleans up SQL resources (materialized tables)
 * - Provides cleanup on component unmount
 * - Coordinates cleanup between multiple cleanup operations
 * - Prevents orphaned resources
 */
export class CleanupManager {
  private queryExecutionService: QueryExecutionService;

  constructor(queryExecutionService: QueryExecutionService) {
    this.queryExecutionService = queryExecutionService;
  }

  /**
   * Type guard to check if a node implements the dispose pattern.
   */
  private isDisposable(
    node: QueryNode,
  ): node is QueryNode & {dispose: () => void} {
    return 'dispose' in node && typeof node.dispose === 'function';
  }

  /**
   * Cleans up a single node's resources.
   * Handles both synchronous (JS) and asynchronous (SQL) cleanup.
   *
   * @param node The node to clean up
   */
  async cleanupNode(node: QueryNode): Promise<void> {
    // First: Synchronous cleanup (intervals, subscriptions, etc.)
    if (this.isDisposable(node)) {
      try {
        node.dispose();
      } catch (e) {
        console.error(
          `Failed to dispose resources for node ${node.nodeId}:`,
          e,
        );
        // Continue - don't block cleanup on individual failures
      }
    }

    // Second: Asynchronous cleanup (materialized tables)
    if (node.state.materialized === true) {
      try {
        await this.queryExecutionService.dropMaterialization(node);
      } catch (e) {
        console.error(
          `Failed to drop materialization for node ${node.nodeId}:`,
          e,
        );
        // Continue - don't block cleanup on individual failures
      }
    }

    // Third: Clean up cached query hash to prevent memory leak
    this.queryExecutionService.deleteNodeHash(node);
  }

  /**
   * Cleans up multiple nodes' resources in parallel.
   *
   * @param nodes The nodes to clean up
   */
  async cleanupNodes(nodes: QueryNode[]): Promise<void> {
    // First: Synchronous cleanup (dispose) for all nodes
    for (const node of nodes) {
      if (this.isDisposable(node)) {
        try {
          node.dispose();
        } catch (e) {
          console.error(
            `Failed to dispose resources for node ${node.nodeId}:`,
            e,
          );
          // Continue - don't block cleanup on individual failures
        }
      }
    }

    // Second: Asynchronous cleanup (materialized tables) in parallel
    const materialized = nodes.filter(
      (node) => node.state.materialized === true,
    );

    if (materialized.length > 0) {
      // Drop all materializations in parallel
      const results = await Promise.allSettled(
        materialized.map((node) =>
          this.queryExecutionService.dropMaterialization(node),
        ),
      );

      // Log failures but don't throw - cleanup should be best-effort
      results.forEach((result, index) => {
        if (result.status === 'rejected') {
          console.error(
            `Failed to drop materialization for node ${materialized[index].nodeId}:`,
            result.reason,
          );
        }
      });
    }

    // Third: Clean up cached query hashes for all nodes to prevent memory leak
    for (const node of nodes) {
      this.queryExecutionService.deleteNodeHash(node);
    }
  }

  /**
   * Cleans up all resources for the entire graph.
   * Used when clearing all nodes or on component unmount.
   *
   * @param allNodes All nodes in the graph
   */
  async cleanupAll(allNodes: QueryNode[]): Promise<void> {
    await this.cleanupNodes(allNodes);
  }
}
