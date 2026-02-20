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

import {QueryResult} from '../../../base/query_slot';
import {Row, SqlValue} from '../../../trace_processor/query_result';
import {AggregateFunction, Filter, GroupPath, IdBasedTree} from './model';

/**
 * Data source interface for DataGrid.
 *
 * Uses a slot-like API where each use* method:
 * - Takes the current model/parameters
 * - Returns the current state (data, isPending, isFresh)
 * - Automatically schedules fetches when parameters change
 *
 * Call these methods on every render cycle - they handle caching internally.
 */
export interface DataSource {
  /**
   * Fetch rows for the current model state.
   * Call every render with the current model to get rows and trigger updates.
   */
  useRows(model: DataSourceModel): DataSourceRows;

  /**
   * Fetch aggregate summaries (aggregates across all filtered rows).
   * Returns summaries for columns with aggregate functions or pivot aggregates.
   */
  useAggregateSummaries(model: DataSourceModel): QueryResult<Row>;

  /**
   * Fetch distinct values for a column (for filter dropdowns).
   * Pass undefined to skip fetching.
   */
  useDistinctValues(
    column: string | undefined,
  ): QueryResult<readonly SqlValue[]>;

  /**
   * Fetch parameter keys for a parameterized column prefix (e.g., 'args' -> ['foo', 'bar']).
   * Pass undefined to skip fetching.
   */
  useParameterKeys(prefix: string | undefined): QueryResult<readonly string[]>;

  /**
   * Export all data with current filters/sorting applied (no pagination).
   * Returns a promise that resolves to all filtered and sorted rows.
   */
  exportData(model: DataSourceModel): Promise<readonly Row[]>;
}

// Common fields shared across all data source modes
interface DataSourceModelBase {
  // Active filters to apply to the data
  readonly filters?: readonly Filter[];

  // Pagination settings (offset and limit for the current page)
  readonly pagination?: {
    readonly offset: number;
    readonly limit: number;
  };

  // Sorting specification (by output alias)
  readonly sort?: {
    readonly alias: string;
    readonly direction: 'ASC' | 'DESC';
  };
}

// Flat mode: simple column selection
export interface FlatModel extends DataSourceModelBase {
  readonly mode: 'flat';
  readonly columns: readonly {
    readonly field: string;
    readonly alias: string;
    readonly aggregate?: AggregateFunction;
  }[];
}

// Pivot mode: grouped/aggregated views
export interface PivotModel extends DataSourceModelBase {
  readonly mode: 'pivot';
  // Columns to group by (hierarchy levels)
  readonly groupBy: readonly {
    readonly field: string;
    readonly alias: string;
  }[];
  // Aggregate expressions
  readonly aggregates: readonly (
    | {
        readonly function: 'COUNT';
        readonly alias: string;
      }
    | {
        readonly function: AggregateFunction;
        readonly field: string;
        readonly alias: string;
      }
  )[];
  // How to display grouped data: 'flat' shows leaf rows only, 'tree' shows
  // hierarchical structure with expand/collapse
  readonly groupDisplay?: 'flat' | 'tree';
  // Allowlist mode: only these group paths are expanded
  readonly expandedGroups?: readonly GroupPath[];
  // Denylist mode: all nodes expanded except these group paths
  readonly collapsedGroups?: readonly GroupPath[];
}

// Tree mode: hierarchical data using id/parent_id columns
export interface TreeModel extends DataSourceModelBase {
  readonly mode: 'tree';
  // Columns to display
  readonly columns: readonly {
    readonly field: string;
    readonly alias: string;
  }[];
  // Tree configuration from IdBasedTree
  readonly tree: IdBasedTree;
}

export type DataSourceModel = FlatModel | PivotModel | TreeModel;

export interface DataSourceRows {
  // The total number of rows available in the dataset
  readonly totalRows?: number;

  // The offset of the first row in this batch
  readonly rowOffset?: number;

  // The actual row data for this batch
  readonly rows?: readonly Row[];

  // Whether the data is currently being fetched
  readonly isPending: boolean;
}
