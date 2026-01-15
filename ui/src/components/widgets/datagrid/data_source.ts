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
import {DataGridColumn, Filter, Pagination, PivotModel, SortBy} from './model';

export interface DataSource {
  readonly result?: DataSourceResult;

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
  readonly columns?: readonly DataGridColumn[];
  readonly sorting?: SortBy;
  readonly filters?: readonly Filter[];
  readonly pagination?: Pagination;
  readonly pivot?: PivotModel;
  readonly distinctValuesColumns?: ReadonlySet<string>;
  // Request parameter keys for these parameterized column prefixes (e.g., 'args', 'skills')
  readonly parameterKeyColumns?: ReadonlySet<string>;
}

export interface DataSourceResult {
  readonly totalRows: number;
  readonly rowOffset: number;
  readonly rows: readonly Row[];
  readonly isLoading?: boolean;
  readonly distinctValues?: ReadonlyMap<string, readonly SqlValue[]>;
  // Available parameter keys for parameterized columns (e.g., for 'args' -> ['foo', 'bar'])
  readonly parameterKeys?: ReadonlyMap<string, readonly string[]>;
  // Computed aggregate totals for each aggregate column (grand total across all filtered rows)
  readonly aggregateTotals?: ReadonlyMap<string, SqlValue>;
}
