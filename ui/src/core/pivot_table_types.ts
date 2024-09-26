// Copyright (C) 2024 The Android Open Source Project
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

import {SortDirection} from '../base/comparison_utils';
import {AreaSelection} from '../public/selection';
import {ColumnType} from '../trace_processor/query_result';

// Auxiliary metadata needed to parse the query result, as well as to render it
// correctly. Generated together with the text of query and passed without the
// change to the query response.

export interface PivotTableQueryMetadata {
  pivotColumns: TableColumn[];
  aggregationColumns: Aggregation[];
  countIndex: number;
}
// Everything that's necessary to run the query for pivot table

export interface PivotTableQuery {
  text: string;
  metadata: PivotTableQueryMetadata;
}
// Pivot table query result

export interface PivotTableResult {
  // Hierarchical pivot structure on top of rows
  tree: PivotTree;
  // Copy of the query metadata from the request, bundled up with the query
  // result to ensure the correct rendering.
  metadata: PivotTableQueryMetadata;
}
// Input parameters to check whether the pivot table needs to be re-queried.

export interface PivotTableState {
  // Currently selected area, if null, pivot table is not going to be visible.
  selectionArea?: AreaSelection;

  // Query response
  queryResult: PivotTableResult | undefined;

  // Selected pivots for tables other than slice.
  // Because of the query generation, pivoting happens first on non-slice
  // pivots; therefore, those can't be put after slice pivots. In order to
  // maintain the separation more clearly, slice and non-slice pivots are
  // located in separate arrays.
  selectedPivots: TableColumn[];

  // Selected aggregation columns. Stored same way as pivots.
  selectedAggregations: Aggregation[];

  // Whether the pivot table results should be constrained to the selected area.
  constrainToArea: boolean;
}

// Node in the hierarchical pivot tree. Only leaf nodes contain data from the
// query result.

export interface PivotTree {
  // Whether the node should be collapsed in the UI, false by default and can
  // be toggled with the button.
  isCollapsed: boolean;

  // Non-empty only in internal nodes.
  children: Map<ColumnType, PivotTree>;
  aggregates: ColumnType[];

  // Non-empty only in leaf nodes.
  rows: ColumnType[][];
}

export type AggregationFunction = 'COUNT' | 'SUM' | 'MIN' | 'MAX' | 'AVG';
// Queried "table column" is either:
// 1. A real one, represented as object with table and column name.
// 2. Pseudo-column 'count' that's rendered as '1' in SQL to use in queries like
// `select sum(1), name from slice group by name`.

export interface RegularColumn {
  kind: 'regular';
  table: string;
  column: string;
}

export interface ArgumentColumn {
  kind: 'argument';
  argument: string;
}

export type TableColumn = RegularColumn | ArgumentColumn;

export function tableColumnEquals(t1: TableColumn, t2: TableColumn): boolean {
  switch (t1.kind) {
    case 'argument': {
      return t2.kind === 'argument' && t1.argument === t2.argument;
    }
    case 'regular': {
      return (
        t2.kind === 'regular' &&
        t1.table === t2.table &&
        t1.column === t2.column
      );
    }
  }
}

export function toggleEnabled<T>(
  compare: (fst: T, snd: T) => boolean,
  arr: T[],
  column: T,
  enabled: boolean,
): void {
  if (enabled && arr.find((value) => compare(column, value)) === undefined) {
    arr.push(column);
  }
  if (!enabled) {
    const index = arr.findIndex((value) => compare(column, value));
    if (index !== -1) {
      arr.splice(index, 1);
    }
  }
}

export interface Aggregation {
  aggregationFunction: AggregationFunction;
  column: TableColumn;

  // If the aggregation is sorted, the field contains a sorting direction.
  sortDirection?: SortDirection;
}

export function aggregationEquals(agg1: Aggregation, agg2: Aggregation) {
  return new EqualsBuilder(agg1, agg2)
    .comparePrimitive((agg) => agg.aggregationFunction)
    .compare(tableColumnEquals, (agg) => agg.column)
    .equals();
}

// Used to convert TableColumn to a string in order to store it in a Map, as
// ES6 does not support compound Set/Map keys. This function should only be used
// for interning keys, and does not have any requirements beyond different
// TableColumn objects mapping to different strings.

export function columnKey(tableColumn: TableColumn): string {
  switch (tableColumn.kind) {
    case 'argument': {
      return `argument:${tableColumn.argument}`;
    }
    case 'regular': {
      return `${tableColumn.table}.${tableColumn.column}`;
    }
  }
}

export function aggregationKey(aggregation: Aggregation): string {
  return `${aggregation.aggregationFunction}:${columnKey(aggregation.column)}`;
}

export const COUNT_AGGREGATION: Aggregation = {
  aggregationFunction: 'COUNT',
  // Exact column is ignored for count aggregation because it does not matter
  // what to count, use empty strings.
  column: {kind: 'regular', table: '', column: ''},
};

// Simple builder-style class to implement object equality more succinctly.
class EqualsBuilder<T> {
  result = true;
  first: T;
  second: T;

  constructor(first: T, second: T) {
    this.first = first;
    this.second = second;
  }

  comparePrimitive(getter: (arg: T) => string | number): EqualsBuilder<T> {
    if (this.result) {
      this.result = getter(this.first) === getter(this.second);
    }
    return this;
  }

  compare<S>(
    comparator: (first: S, second: S) => boolean,
    getter: (arg: T) => S,
  ): EqualsBuilder<T> {
    if (this.result) {
      this.result = comparator(getter(this.first), getter(this.second));
    }
    return this;
  }

  equals(): boolean {
    return this.result;
  }
}
