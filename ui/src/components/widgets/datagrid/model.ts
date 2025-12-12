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

import {SqlValue} from '../../../trace_processor/query_result';

export type AggregationFunction =
  | 'SUM'
  | 'AVG'
  | 'COUNT'
  | 'MIN'
  | 'MAX'
  | 'ANY';

export interface FilterValue {
  readonly column: string;
  readonly op: '=' | '!=' | '<' | '<=' | '>' | '>=' | 'glob' | 'not glob';
  readonly value: SqlValue;
}

export interface FilterIn {
  readonly column: string;
  readonly op: 'in' | 'not in';
  readonly value: ReadonlyArray<SqlValue>;
}

export interface FilterNull {
  readonly column: string;
  readonly op: 'is null' | 'is not null';
}

export type DataGridFilter = FilterValue | FilterNull | FilterIn;

export interface SortByColumn {
  readonly column: string;
  readonly direction: 'ASC' | 'DESC';
}

export interface Unsorted {
  readonly direction: 'UNSORTED';
}

export type Sorting = SortByColumn | Unsorted;

export interface DataSourceResult {
  readonly totalRows: number;
  readonly rowOffset: number;
  readonly rows: ReadonlyArray<RowDef>;
  readonly isLoading?: boolean;
  readonly distinctValues?: ReadonlyMap<string, readonly SqlValue[]>;
  // Available parameter keys for parameterized columns (e.g., for 'args' -> ['foo', 'bar'])
  readonly parameterKeys?: ReadonlyMap<string, readonly string[]>;
  // Computed aggregate totals for each aggregate column (grand total across all filtered rows)
  readonly aggregateTotals?: ReadonlyMap<string, SqlValue>;
}

export type RowDef = {[key: string]: SqlValue};

export interface Pagination {
  readonly offset: number;
  readonly limit: number;
}

/**
 * A pivot value that aggregates a specific column.
 */
interface PivotValueWithCol {
  readonly col: string;
  readonly func: 'SUM' | 'AVG' | 'MIN' | 'MAX' | 'ANY';
}

/**
 * A pivot value that counts rows (doesn't need a specific column).
 */
interface PivotValueCount {
  readonly func: 'COUNT';
}

export type PivotValue = PivotValueWithCol | PivotValueCount;

/**
 * Model for pivot/grouping state of the data grid.
 */
export interface PivotModel {
  // Columns to group by, in order
  readonly groupBy: ReadonlyArray<string>;

  // Aggregated values to compute - keys are alias names, values define the aggregation
  readonly values: {
    readonly [key: string]: PivotValue;
  };

  // When set, shows raw rows filtered by these groupBy column values.
  // This allows drilling down into a specific pivot group to see the
  // underlying data. The keys are the groupBy column names.
  readonly drillDown?: RowDef;
}

/**
 * A column in the DataGridModel, with optional aggregation.
 */
export interface DataGridColumn {
  readonly column: string;
  // Optional aggregation function to compute for this column.
  // Results are returned in DataSourceResult.aggregateTotals.
  readonly aggregation?: AggregationFunction;
}

export interface DataGridModel {
  readonly columns?: ReadonlyArray<DataGridColumn>;
  readonly sorting?: Sorting;
  readonly filters?: ReadonlyArray<DataGridFilter>;
  readonly pagination?: Pagination;
  readonly pivot?: PivotModel;
  readonly distinctValuesColumns?: ReadonlySet<string>;
  // Request parameter keys for these parameterized column prefixes (e.g., 'args', 'skills')
  readonly parameterKeyColumns?: ReadonlySet<string>;
}

export interface DataGridDataSource {
  readonly rows?: DataSourceResult;
  readonly isLoading?: boolean;
  notifyUpdate(model: DataGridModel): void;

  /**
   * Export all data with current filters/sorting applied.
   * Returns a promise that resolves to all filtered and sorted rows.
   */
  exportData(): Promise<readonly RowDef[]>;
}
