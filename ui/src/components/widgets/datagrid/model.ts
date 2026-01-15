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

export type AggregationFunction =
  | 'SUM'
  | 'AVG'
  | 'COUNT'
  | 'MIN'
  | 'MAX'
  | 'ANY';

export interface ValueFilter {
  readonly column: string;
  readonly op: '=' | '!=' | '<' | '<=' | '>' | '>=' | 'glob' | 'not glob';
  readonly value: SqlValue;
}

export interface InFilter {
  readonly column: string;
  readonly op: 'in' | 'not in';
  readonly value: ReadonlyArray<SqlValue>;
}

export interface NullFilter {
  readonly column: string;
  readonly op: 'is null' | 'is not null';
}

export type Filter = ValueFilter | NullFilter | InFilter;

export interface SortByColumn {
  readonly column: string;
  readonly direction: 'ASC' | 'DESC';
}

export interface Unsorted {
  readonly direction: 'UNSORTED';
}

export type SortBy = SortByColumn | Unsorted;

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
  readonly drillDown?: Row;
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
