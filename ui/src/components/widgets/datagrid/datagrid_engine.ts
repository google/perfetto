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
import {Filter} from './model';

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
export interface DatagridEngine {
  /**
   * Fetch rows for the current model state.
   * Call every render with the current model to get rows and trigger updates.
   */
  useRows(model: DataSourceModel): DataSourceRows;

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
   * Fetch aggregate summaries (aggregates across all filtered rows).
   * Returns summaries for columns with aggregate functions or pivot aggregates.
   */
  useAggregateSummaries(model: DataSourceModel): QueryResult<Row>;

  /**
   * Export all data with current filters/sorting applied.
   * Returns a promise that resolves to all filtered and sorted rows.
   */
  exportData(): Promise<readonly Row[]>;
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

  // Columns for which to fetch distinct values (for filter dropdowns)
  readonly distinctValuesColumns?: ReadonlySet<string>;

  // Parameterized column prefixes for which to fetch available keys (e.g.,
  // 'args')
  readonly parameterKeyColumns?: ReadonlySet<string>;
}

// Flat mode: simple column selection
export interface FlatModel extends DataSourceModelBase {
  readonly mode: 'flat';
  readonly columns: readonly {
    readonly field: string;
    readonly alias: string;
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
        readonly function: 'SUM' | 'AVG' | 'MIN' | 'MAX' | 'ANY';
        readonly field: string;
        readonly alias: string;
      }
  )[];
  // How to display grouped data: 'flat' shows leaf rows only, 'tree' shows
  // hierarchical structure with expand/collapse
  readonly groupDisplay?: 'flat' | 'tree';
  // Allowlist mode: only these node IDs are expanded
  readonly expandedIds?: ReadonlySet<bigint>;
  // Denylist mode: all nodes expanded except these IDs
  readonly collapsedIds?: ReadonlySet<bigint>;
}

export type DataSourceModel = FlatModel | PivotModel;

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
