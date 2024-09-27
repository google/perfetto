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

import {
  PivotTableQuery,
  PivotTableQueryMetadata,
  PivotTableResult,
  PivotTableState,
  COUNT_AGGREGATION,
  TableColumn,
  toggleEnabled,
  tableColumnEquals,
  AggregationFunction,
} from './pivot_table_types';
import {AreaSelection} from '../public/selection';
import {
  aggregationIndex,
  generateQueryFromState,
} from './pivot_table_query_generator';
import {Aggregation, PivotTree} from './pivot_table_types';
import {Engine} from '../trace_processor/engine';
import {ColumnType} from '../trace_processor/query_result';
import {SortDirection} from '../base/comparison_utils';
import {assertTrue} from '../base/logging';
import {featureFlags} from './feature_flags';

export const PIVOT_TABLE_REDUX_FLAG = featureFlags.register({
  id: 'pivotTable',
  name: 'Pivot tables V2',
  description: 'Second version of pivot table',
  defaultValue: true,
});

function expectNumber(value: ColumnType): number {
  if (typeof value === 'number') {
    return value;
  } else if (typeof value === 'bigint') {
    return Number(value);
  }
  throw new Error(`number or bigint was expected, got ${typeof value}`);
}

// Auxiliary class to build the tree from query response.
export class PivotTableTreeBuilder {
  private readonly root: PivotTree;
  queryMetadata: PivotTableQueryMetadata;

  get pivotColumnsCount(): number {
    return this.queryMetadata.pivotColumns.length;
  }

  get aggregateColumns(): Aggregation[] {
    return this.queryMetadata.aggregationColumns;
  }

  constructor(queryMetadata: PivotTableQueryMetadata, firstRow: ColumnType[]) {
    this.queryMetadata = queryMetadata;
    this.root = this.createNode(firstRow);
    let tree = this.root;
    for (let i = 0; i + 1 < this.pivotColumnsCount; i++) {
      const value = firstRow[i];
      tree = this.insertChild(tree, value, this.createNode(firstRow));
    }
    tree.rows.push(firstRow);
  }

  // Add incoming row to the tree being built.
  ingestRow(row: ColumnType[]) {
    let tree = this.root;
    this.updateAggregates(tree, row);
    for (let i = 0; i + 1 < this.pivotColumnsCount; i++) {
      const nextTree = tree.children.get(row[i]);
      if (nextTree === undefined) {
        // Insert the new node into the tree, and make variable `tree` point
        // to the newly created node.
        tree = this.insertChild(tree, row[i], this.createNode(row));
      } else {
        this.updateAggregates(nextTree, row);
        tree = nextTree;
      }
    }
    tree.rows.push(row);
  }

  build(): PivotTree {
    return this.root;
  }

  updateAggregates(tree: PivotTree, row: ColumnType[]) {
    const countIndex = this.queryMetadata.countIndex;
    const treeCount =
      countIndex >= 0 ? expectNumber(tree.aggregates[countIndex]) : 0;
    const rowCount =
      countIndex >= 0
        ? expectNumber(
            row[aggregationIndex(this.pivotColumnsCount, countIndex)],
          )
        : 0;

    for (let i = 0; i < this.aggregateColumns.length; i++) {
      const agg = this.aggregateColumns[i];

      const currAgg = tree.aggregates[i];
      const childAgg = row[aggregationIndex(this.pivotColumnsCount, i)];
      if (typeof currAgg === 'number' && typeof childAgg === 'number') {
        switch (agg.aggregationFunction) {
          case 'SUM':
          case 'COUNT':
            tree.aggregates[i] = currAgg + childAgg;
            break;
          case 'MAX':
            tree.aggregates[i] = Math.max(currAgg, childAgg);
            break;
          case 'MIN':
            tree.aggregates[i] = Math.min(currAgg, childAgg);
            break;
          case 'AVG': {
            const currSum = currAgg * treeCount;
            const addSum = childAgg * rowCount;
            tree.aggregates[i] = (currSum + addSum) / (treeCount + rowCount);
            break;
          }
        }
      }
    }
    tree.aggregates[this.aggregateColumns.length] = treeCount + rowCount;
  }

  // Helper method that inserts child node into the tree and returns it, used
  // for more concise modification of local variable pointing to the current
  // node being built.
  insertChild(tree: PivotTree, key: ColumnType, child: PivotTree): PivotTree {
    tree.children.set(key, child);

    return child;
  }

  // Initialize PivotTree from a row.
  createNode(row: ColumnType[]): PivotTree {
    const aggregates = [];

    for (let j = 0; j < this.aggregateColumns.length; j++) {
      aggregates.push(row[aggregationIndex(this.pivotColumnsCount, j)]);
    }
    aggregates.push(
      row[
        aggregationIndex(this.pivotColumnsCount, this.aggregateColumns.length)
      ],
    );

    return {
      isCollapsed: false,
      children: new Map(),
      aggregates,
      rows: [],
    };
  }
}

function createEmptyQueryResult(
  metadata: PivotTableQueryMetadata,
): PivotTableResult {
  return {
    tree: {
      aggregates: [],
      isCollapsed: false,
      children: new Map(),
      rows: [],
    },
    metadata,
  };
}

// Controller responsible for showing the panel with pivot table, as well as
// executing its queries and post-processing query results.
export class PivotTableManager {
  state: PivotTableState = createEmptyPivotTableState();

  constructor(private engine: Engine) {}

  setSelectionArea(area: AreaSelection) {
    if (!PIVOT_TABLE_REDUX_FLAG.get()) {
      return;
    }
    this.state.selectionArea = area;
    this.refresh();
  }

  addAggregation(aggregation: Aggregation, after: number) {
    this.state.selectedAggregations.splice(after, 0, aggregation);
    this.refresh();
  }

  removeAggregation(index: number) {
    this.state.selectedAggregations.splice(index, 1);
    this.refresh();
  }

  setPivotSelected(args: {column: TableColumn; selected: boolean}) {
    toggleEnabled(
      tableColumnEquals,
      this.state.selectedPivots,
      args.column,
      args.selected,
    );
    this.refresh();
  }

  setAggregationFunction(index: number, fn: AggregationFunction) {
    this.state.selectedAggregations[index].aggregationFunction = fn;
    this.refresh();
  }

  setSortColumn(aggregationIndex: number, order: SortDirection) {
    this.state.selectedAggregations = this.state.selectedAggregations.map(
      (agg, index) => ({
        column: agg.column,
        aggregationFunction: agg.aggregationFunction,
        sortDirection: index === aggregationIndex ? order : undefined,
      }),
    );
    this.refresh();
  }

  setOrder(from: number, to: number, direction: DropDirection) {
    const pivots = this.state.selectedPivots;
    this.state.selectedPivots = performReordering(
      computeIntervals(pivots.length, from, to, direction),
      pivots,
    );
    this.refresh();
  }

  setAggregationOrder(from: number, to: number, direction: DropDirection) {
    const aggregations = this.state.selectedAggregations;
    this.state.selectedAggregations = performReordering(
      computeIntervals(aggregations.length, from, to, direction),
      aggregations,
    );
    this.refresh();
  }

  setConstrainedToArea(constrain: boolean) {
    this.state.constrainToArea = constrain;
    this.refresh();
  }

  private refresh() {
    this.state.queryResult = undefined;
    if (!PIVOT_TABLE_REDUX_FLAG.get()) {
      return;
    }
    this.processQuery(generateQueryFromState(this.state));
  }

  private async processQuery(query: PivotTableQuery) {
    const result = await this.engine.query(query.text);
    try {
      await result.waitAllRows();
    } catch {
      // waitAllRows() frequently throws an exception, which is ignored in
      // its other calls, so it's ignored here as well.
    }

    const columns = result.columns();

    const it = result.iter({});
    function nextRow(): ColumnType[] {
      const row: ColumnType[] = [];
      for (const column of columns) {
        row.push(it.get(column));
      }
      it.next();
      return row;
    }

    if (!it.valid()) {
      // Iterator is invalid after creation; means that there are no rows
      // satisfying filtering criteria. Return an empty tree.
      this.state.queryResult = createEmptyQueryResult(query.metadata);
      return;
    }

    const treeBuilder = new PivotTableTreeBuilder(query.metadata, nextRow());
    while (it.valid()) {
      treeBuilder.ingestRow(nextRow());
    }
    this.state.queryResult = {
      tree: treeBuilder.build(),
      metadata: query.metadata,
    };
  }
}

function createEmptyPivotTableState(): PivotTableState {
  return {
    queryResult: undefined,
    selectedPivots: [
      {
        kind: 'regular',
        table: '_slice_with_thread_and_process_info',
        column: 'name',
      },
    ],
    selectedAggregations: [
      {
        aggregationFunction: 'SUM',
        column: {
          kind: 'regular',
          table: '_slice_with_thread_and_process_info',
          column: 'dur',
        },
        sortDirection: 'DESC',
      },
      {
        aggregationFunction: 'SUM',
        column: {
          kind: 'regular',
          table: '_slice_with_thread_and_process_info',
          column: 'thread_dur',
        },
      },
      COUNT_AGGREGATION,
    ],
    constrainToArea: true,
  };
}

// Drag&Drop logic

export type DropDirection = 'left' | 'right';

export interface Interval {
  from: number;
  to: number;
}

/*
 * When a drag'n'drop is performed in a linear sequence, the resulting reordered
 * array will consist of several contiguous subarrays of the original glued
 * together.
 *
 * This function implements the computation of these intervals.
 *
 * The drag'n'drop operation performed is as follows: in the sequence with given
 * length, the element with index `dragFrom` is dropped on the `direction` to
 * the element `dragTo`.
 */

export function computeIntervals(
  length: number,
  dragFrom: number,
  dragTo: number,
  direction: DropDirection,
): Interval[] {
  assertTrue(dragFrom !== dragTo);

  if (dragTo < dragFrom) {
    const prefixLen = direction == 'left' ? dragTo : dragTo + 1;
    return [
      // First goes unchanged prefix.
      {from: 0, to: prefixLen},
      // Then goes dragged element.
      {from: dragFrom, to: dragFrom + 1},
      // Then goes suffix up to dragged element (which has already been moved).
      {from: prefixLen, to: dragFrom},
      // Then the rest of an array.
      {from: dragFrom + 1, to: length},
    ];
  }

  // Other case: dragTo > dragFrom
  const prefixLen = direction == 'left' ? dragTo : dragTo + 1;
  return [
    {from: 0, to: dragFrom},
    {from: dragFrom + 1, to: prefixLen},
    {from: dragFrom, to: dragFrom + 1},
    {from: prefixLen, to: length},
  ];
}

export function performReordering<T>(intervals: Interval[], arr: T[]): T[] {
  const result: T[] = [];

  for (const interval of intervals) {
    result.push(...arr.slice(interval.from, interval.to));
  }

  return result;
}
