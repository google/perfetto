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

import {Row, SqlValue} from '../../../trace_processor/query_result';
import {Column, Filter, Pivot} from './model';

export interface Pagination {
  readonly offset: number;
  readonly limit: number;
}

export interface DataSource {
  // The row data for the current data grid state (filters, sorting, pagination,
  // etc)
  readonly rows?: DataSourceRows;

  // Available distinct values for specified columns (for filter dropdowns)
  readonly distinctValues?: ReadonlyMap<string, readonly SqlValue[]>;

  // Available parameter keys for parameterized columns (e.g., for 'args' ->
  // ['foo', 'bar'])
  readonly parameterKeys?: ReadonlyMap<string, readonly string[]>;

  // Computed aggregate totals for each aggregate column (grand total across all
  // filtered rows)
  readonly aggregateTotals?: ReadonlyMap<string, SqlValue>;

  // Whether the data source is currently loading data/updating.
  readonly isLoading?: boolean;

  // Called when the data grid parameters change (sorting, filtering,
  // pagination, etc), which might trigger a data reload.
  notify(model: DataSourceModel): void;

  // Export all data with current filters/sorting applied. Returns a promise
  // that resolves to all filtered and sorted rows.
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
