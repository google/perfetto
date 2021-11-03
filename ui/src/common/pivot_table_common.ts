// Copyright (C) 2021 The Android Open Source Project
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

import {getHiddenPivotAlias} from './pivot_table_query_generator';
import {Row} from './query_result';

export const AVAILABLE_TABLES = ['slice'];
export const AVAILABLE_AGGREGATIONS = ['COUNT', 'SUM', 'AVG', 'MIN', 'MAX'];
export const WHERE_FILTERS = ['slice.dur != -1'];
export const SLICE_STACK_HELPER_COLUMNS =
    ['depth', 'stack_id', 'parent_stack_id'];
export const SLICE_STACK_COLUMN = 'name (stack)';
export const DEFAULT_PIVOT_TABLE_ID = 'pivot-table';
export const SLICE_AGGREGATION_PIVOT_TABLE_ID = 'pivot-table-slices';

export interface AggregationAttrs {
  tableName: string;
  columnName: string;
  aggregation: string;
  order: string;
}

export interface PivotAttrs {
  tableName: string;
  columnName: string;
  isStackPivot: boolean;
}

export interface TableAttrs {
  tableName: string;
  columns: string[];
}

export interface ColumnAttrs {
  name: string;
  index: number;
  tableName: string;
  columnName: string;
  aggregation?: string;
  order?: string;
  isStackColumn: boolean;
}

export interface RowAttrs {
  row: Row;
  expandableColumns: Set<string>;  // Columns at which the row can be expanded.
  expandedRows: Map<string, {
    isExpanded: boolean,
    rows: RowAttrs[]
  }>;  // Contains the expanded rows of each expanded expandableColumn.
  whereFilters: Map<string, string>;  // Where filters of each column that
                                      // joins the row with its parent.
  loadingColumn?: string;
  depth: number;
}

export interface SubQueryAttrs {
  rowIndices: number[];
  columnIdx: number;
  value: string;
  expandedRowColumns: string[];
}

export interface SubQueryAttrs {
  rowIndices: number[];
  columnIdx: number;
  value: string;
}

export interface PivotTableQueryResponse {
  columns: ColumnAttrs[];
  error?: string;
  durationMs: number;
  rows: RowAttrs[];
  totalAggregations?: Row;
}

// Determine if the column provided is a stack column that can be expanded
// into descendants.
export function isStackPivot(tableName: string, columnName: string) {
  if (tableName === 'slice' && columnName === SLICE_STACK_COLUMN) {
    return true;
  }
  return false;
}

// Get the helper columns that are needed to expand a stack pivot.
export function getHiddenStackHelperColumns(pivot: PivotAttrs) {
  const hiddenColumns: Array<{pivotAttrs: PivotAttrs, columnAlias: string}> =
      [];
  if (pivot.tableName === 'slice') {
    for (const column of SLICE_STACK_HELPER_COLUMNS) {
      const pivotAttrs = {
        tableName: pivot.tableName,
        columnName: column,
        isStackPivot: false
      };
      hiddenColumns.push(
          {pivotAttrs, columnAlias: getHiddenPivotAlias(pivotAttrs)});
    }
  }
  return hiddenColumns;
}

// Removing unnecessary columns from table and adding stack column if it exists.
export function removeHiddenAndAddStackColumns(
    tableName: string, columns: string[]) {
  if (tableName === 'slice') {
    // Removing "cat" and "slice_id" to maintain the original schema of the
    // slice table that's compatible with descendant_slice_by_stack table.
    columns = columns.filter(
        column => ['stack_id', 'parent_stack_id', 'cat', 'slice_id'].includes(
                      column) === false);
    columns.push(SLICE_STACK_COLUMN);
  }
  return columns;
}

// Get a list of tables that include the descendants that need to be queried.
export function getDescendantsTables(pivots: PivotAttrs[], stackId: string) {
  const descendantsTables = [...AVAILABLE_TABLES];
  let descendantsTable = 'undefined_table';
  let replaceIdx = -1;
  if (pivots.length > 0 && pivots[0].tableName === 'slice') {
    // Replace slice table with descendants table.
    descendantsTable = `descendant_slice_by_stack(${stackId}) AS slice`;
    replaceIdx = descendantsTables.indexOf('slice');
    if (replaceIdx === -1) {
      throw Error('Slice table not found.');
    }
  }
  if (pivots.length === 0 ||
      !isStackPivot(pivots[0].tableName, pivots[0].columnName) ||
      replaceIdx === -1) {
    throw Error('Invalid Arguments to "getDescendantsTables"');
  }
  descendantsTables[replaceIdx] = descendantsTable;
  return descendantsTables;
}

// Get the stack id column in the stack pivot table.
export function getStackColumn(pivot: PivotAttrs) {
  if (pivot.tableName === 'slice') {
    return {tableName: 'slice', columnName: 'stack_id', isStackPivot: false};
  }
  throw Error('"getStackColumn" called on pivot that is not a stack column.');
}

// Get the parent stack id column in the stack pivot table.
export function getParentStackColumn(pivot: PivotAttrs) {
  if (pivot.tableName === 'slice') {
    return {
      tableName: 'slice',
      columnName: 'parent_stack_id',
      isStackPivot: false
    };
  }
  throw Error(
      '"getParentStackColumn" called on pivot that is not a stack column.');
}

// Get the depth column in the stack pivot table.
export function getStackDepthColumn(pivot: PivotAttrs) {
  if (pivot.tableName === 'slice') {
    return {tableName: 'slice', columnName: 'depth', isStackPivot: false};
  }
  throw Error(
      '"getStackDepthColumn" called on pivot that is not a stack column.');
}

// Get a where filter that restricts the query by the given stack id.
export function getParentStackWhereFilter(pivot: PivotAttrs, stackId: string) {
  const stackColumn = getStackColumn(pivot);
  return `${stackColumn.tableName}.${stackColumn.columnName} = ${stackId}`;
}
