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

import m from 'mithril';
import {SqlValue} from '../../../trace_processor/query_result';

export type AggregationFunction = 'SUM' | 'AVG' | 'COUNT' | 'MIN' | 'MAX';
export interface ColumnDefinition {
  // Name/id of the column - this should match the key in the data.
  readonly name: string;

  // Human readable title to display instead of the name.
  readonly title?: m.Children;

  // An optional aggregation for data in this column displayed in the header
  // bar.
  readonly aggregation?: AggregationFunction;

  // Optional extra menu items to add to the header column's context menu.
  readonly headerMenuItems?: m.Children;

  // Optional function that returns extra menu items to add to each data cell's
  // context menu. The function receives the cell value and the complete row
  // data.
  readonly cellMenuItems?: (value: SqlValue, row: RowDef) => m.Children;
}

export interface FilterValue {
  readonly column: string;
  readonly op: '=' | '!=' | '<' | '<=' | '>' | '>=' | 'glob';
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
  readonly aggregates: RowDef;
  readonly isLoading?: boolean;
}

export type RowDef = {[key: string]: SqlValue};

export interface Pagination {
  readonly offset: number;
  readonly limit: number;
}

export interface AggregateSpec {
  readonly col: string;
  readonly func: AggregationFunction;
}

export interface DataGridModel {
  readonly columns?: ReadonlyArray<string>;
  readonly sorting?: Sorting;
  readonly filters?: ReadonlyArray<DataGridFilter>;
  readonly pagination?: Pagination;
  readonly aggregates?: ReadonlyArray<AggregateSpec>;
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

/**
 * Function to format a value as a string for export/clipboard.
 */
export type ValueFormatter = (
  value: SqlValue,
  columnName: string,
  formatHint?: string,
) => string;

/**
 * Compares two arrays of AggregateSpec objects for equality.
 *
 * Two arrays are considered equal if they have the same length and each
 * corresponding AggregateSpec object is equal.
 */
export function areAggregateArraysEqual(
  a?: ReadonlyArray<AggregateSpec>,
  b?: ReadonlyArray<AggregateSpec>,
): boolean {
  // Both undefined or same object- they're equal
  if (a === b) return true;

  // One is undefined, other isn't - they're different
  if (!a || !b) return false;

  // Lengths differ - they're different
  if (a.length !== b.length) return false;

  // Check each element
  for (let i = 0; i < a.length; i++) {
    if (!areAggregatesEqual(a[i], b[i])) {
      return false;
    }
  }

  // All elements are the same
  return true;
}

/**
 * Compares two AggregateSpec objects for equality.
 *
 * Two AggregateSpec objects are considered equal if their `col` and `func`
 * properties are the same.
 */
export function areAggregatesEqual(
  a: AggregateSpec,
  b: AggregateSpec,
): boolean {
  return a.col === b.col && a.func === b.func;
}
