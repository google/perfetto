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
import {Column, Filter, IdBasedTree, Pivot} from './model';

export interface Pagination {
  readonly offset: number;
  readonly limit: number;
}

/**
 * Result type for useRows(), includes the working SQL query for debugging.
 */
export interface RowsQueryResult extends QueryResult<DataSourceRows> {
  // The SQL query used to fetch the rows (useful for debugging)
  readonly query: string;
}

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
  useRows(model: DataSourceModel): RowsQueryResult;

  /**
   * Fetch distinct values for filter dropdowns.
   * Only fetches for columns specified in model.distinctValuesColumns.
   */
  useDistinctValues(
    model: DataSourceModel,
  ): QueryResult<ReadonlyMap<string, readonly SqlValue[]>>;

  /**
   * Fetch parameter keys for parameterized columns (e.g., 'args' -> ['foo', 'bar']).
   * Only fetches for prefixes specified in model.parameterKeyColumns.
   */
  useParameterKeys(
    model: DataSourceModel,
  ): QueryResult<ReadonlyMap<string, readonly string[]>>;

  /**
   * Fetch aggregate totals (grand totals across all filtered rows).
   * Returns totals for columns with aggregate functions or pivot aggregates.
   */
  useAggregateTotals(
    model: DataSourceModel,
  ): QueryResult<ReadonlyMap<string, SqlValue>>;

  /**
   * Export all data with current filters/sorting applied.
   * Returns a promise that resolves to all filtered and sorted rows.
   */
  exportData(): Promise<readonly Row[]>;
}

export interface DataSourceModel {
  // The columns to display, including their sort direction if any
  readonly columns?: readonly Column[];

  // Active filters to apply to the data
  readonly filters?: readonly Filter[];

  // Pagination settings (offset and limit for the current page)
  readonly pagination?: Pagination;

  // Pivot configuration for grouped/aggregated views
  readonly pivot?: Pivot;

  // ID-based tree configuration using __intrinsic_tree virtual table.
  // Uses explicit id/parent_id columns for tree structure.
  // Mutually exclusive with pivot.
  readonly idBasedTree?: IdBasedTree;

  // Columns for which to fetch distinct values (for filter dropdowns)
  readonly distinctValuesColumns?: ReadonlySet<string>;

  // Parameterized column prefixes for which to fetch available keys (e.g.,
  // 'args')
  readonly parameterKeyColumns?: ReadonlySet<string>;
}

export interface DataSourceRows {
  // The total number of rows available in the dataset
  readonly totalRows: number;

  // The offset of the first row in this batch
  readonly rowOffset: number;

  // The actual row data for this batch
  readonly rows: readonly Row[];
}
