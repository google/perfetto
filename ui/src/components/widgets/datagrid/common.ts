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

export type AggregationFunction =
  | 'SUM'
  | 'AVG'
  | 'COUNT'
  | 'MIN'
  | 'MAX'
  | 'ANY';

/**
 * Represents a column that can be added to the grid via the "Add Column" menu.
 * Used to build hierarchical menus of available columns, supporting both static
 * columns and dynamically-discovered ones (e.g., arg keys, parent chains).
 */
export interface AvailableColumn {
  // Display name for the menu item
  readonly label: string;

  // The column name to add when selected (e.g., "parent.ts", "arg[foo]")
  readonly columnName: string;

  // If defined, this item has a submenu with lazy-loaded children.
  // The function is called when the user expands the submenu.
  readonly getChildren?: () => Promise<AvailableColumn[]>;
}

export type CellRenderer = (value: SqlValue, row: RowDef) => m.Children;
export type CellFormatter = (value: SqlValue, row: RowDef) => string;

export interface ColumnDefinition {
  // Name/id of the column - this should match the key in the data.
  readonly name: string;

  // Human readable title to display instead of the name.
  readonly title?: m.Children;

  // Custom renderer for this column's cells
  readonly cellRenderer?: CellRenderer;

  // Optional value formatter for this column. This is used when exporting
  // data to format the value as a string.
  readonly cellFormatter?: CellFormatter;

  // Optional function that receives default menu item groups and returns
  // the complete menu structure. This allows full control over menu organization.
  // Default groups provided:
  // - sorting: Sort ascending/descending/clear items
  // - filters: Filter options (null filters, equals, contains, etc.)
  // - fitToContent: Fit column to content width
  // - columnManagement: Hide column, manage columns visibility
  readonly contextMenuRenderer?: (builtins: {
    readonly sorting?: m.Children;
    readonly filters?: m.Children;
    readonly fitToContent?: m.Children;
    readonly columnManagement?: m.Children;
  }) => m.Children;

  // Optional function that receives the default filter menu item and returns
  // the complete cell context menu structure. This allows full control over
  // the cell menu organization.
  // Default item provided:
  // - addFilter: "Add filter..." menu item with context-sensitive filter options
  readonly cellContextMenuRenderer?: (
    value: SqlValue,
    row: RowDef,
    builtins: {
      addFilter?: m.Children;
    },
  ) => m.Children;

  // Enable distinct values filtering for this column. When enabled, adds a
  // "Filter by values..." menu item that shows all distinct values. Only
  // enable for columns with low cardinality (e.g., strings, enums).
  readonly distinctValues?: boolean;

  // Control which types of filters are available for this column.
  // - 'numeric': Shows comparison filters (=, !=, <, <=, >, >=) and null filters
  // - 'string': Shows text filters (contains, glob) and equals/null filters
  // - undefined: Shows all applicable filters based on other settings
  readonly filterType?: 'numeric' | 'string';
}

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

// Helper to normalize column input (string or DataGridColumn) to DataGridColumn
export function normalizeColumn(col: string | DataGridColumn): DataGridColumn {
  return typeof col === 'string' ? {column: col} : col;
}

// Helper to get column name from string or DataGridColumn
export function getColumnName(col: string | DataGridColumn): string {
  return typeof col === 'string' ? col : col.column;
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

// Check if the value is numeric (number or bigint)
export function isNumeric(value: SqlValue): value is number | bigint {
  return typeof value === 'number' || typeof value === 'bigint';
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
