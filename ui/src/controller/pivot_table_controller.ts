/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import {Actions} from '../common/actions';
import {DEFAULT_CHANNEL, getCurrentChannel} from '../common/channels';
import {Engine} from '../common/engine';
import {featureFlags} from '../common/feature_flags';
import {ColumnType, STR} from '../common/query_result';
import {
  AreaSelection,
  PivotTableQuery,
  PivotTableQueryMetadata,
  PivotTableResult,
  PivotTableState,
} from '../common/state';
import {globals} from '../frontend/globals';
import {
  aggregationIndex,
  generateQueryFromState,
} from '../frontend/pivot_table_query_generator';
import {Aggregation, PivotTree} from '../frontend/pivot_table_types';

import {Controller} from './controller';

export const PIVOT_TABLE_REDUX_FLAG = featureFlags.register({
  id: 'pivotTable',
  name: 'Pivot tables V2',
  description: 'Second version of pivot table',
  // Enabled in canary and autopush by default.
  defaultValue: getCurrentChannel() !== DEFAULT_CHANNEL,
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
    const rowCount = countIndex >= 0 ?
        expectNumber(
            row[aggregationIndex(this.pivotColumnsCount, countIndex)]) :
        0;

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
    aggregates.push(row[aggregationIndex(
        this.pivotColumnsCount, this.aggregateColumns.length)]);

    return {
      isCollapsed: false,
      children: new Map(),
      aggregates,
      rows: [],
    };
  }
}

function createEmptyQueryResult(metadata: PivotTableQueryMetadata):
    PivotTableResult {
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
export class PivotTableController extends Controller<{}> {
  static detailsCount = 0;
  engine: Engine;
  lastQueryAreaId = '';
  lastQueryAreaTracks = new Set<string>();
  requestedArgumentNames = false;

  constructor(args: {engine: Engine}) {
    super({});
    this.engine = args.engine;
  }

  sameTracks(tracks: Set<string>) {
    if (this.lastQueryAreaTracks.size !== tracks.size) {
      return false;
    }

    // ES6 Set does not have .every method, only Array does.
    for (const track of tracks) {
      if (!this.lastQueryAreaTracks.has(track)) {
        return false;
      }
    }

    return true;
  }

  shouldRerun(state: PivotTableState, selection: AreaSelection) {
    if (state.selectionArea === undefined) {
      return false;
    }

    const newTracks = new Set(globals.state.areas[selection.areaId].tracks);
    if (this.lastQueryAreaId !== state.selectionArea.areaId ||
        !this.sameTracks(newTracks)) {
      this.lastQueryAreaId = state.selectionArea.areaId;
      this.lastQueryAreaTracks = newTracks;
      return true;
    }
    return false;
  }

  async processQuery(query: PivotTableQuery) {
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
      globals.dispatch(Actions.setPivotStateQueryResult(
          {queryResult: createEmptyQueryResult(query.metadata)}));
      return;
    }

    const treeBuilder = new PivotTableTreeBuilder(query.metadata, nextRow());
    while (it.valid()) {
      treeBuilder.ingestRow(nextRow());
    }

    globals.dispatch(Actions.setPivotStateQueryResult(
        {queryResult: {tree: treeBuilder.build(), metadata: query.metadata}}));
    globals.dispatch(Actions.setCurrentTab({tab: 'pivot_table'}));
  }

  async requestArgumentNames() {
    this.requestedArgumentNames = true;
    const result = await this.engine.query(`
      select distinct flat_key from args
    `);
    const it = result.iter({flat_key: STR});

    const argumentNames = [];
    while (it.valid()) {
      argumentNames.push(it.flat_key);
      it.next();
    }

    globals.dispatch(Actions.setPivotTableArgumentNames({argumentNames}));
  }


  run() {
    if (!PIVOT_TABLE_REDUX_FLAG.get()) {
      return;
    }

    if (!this.requestedArgumentNames) {
      this.requestArgumentNames();
    }

    const pivotTableState = globals.state.nonSerializableState.pivotTable;
    const selection = globals.state.currentSelection;

    if (pivotTableState.queryRequested ||
        (selection !== null && selection.kind === 'AREA' &&
         this.shouldRerun(pivotTableState, selection))) {
      globals.dispatch(
          Actions.setPivotTableQueryRequested({queryRequested: false}));
      // Need to re-run the existing query, clear the current result.
      globals.dispatch(Actions.setPivotStateQueryResult({queryResult: null}));
      this.processQuery(generateQueryFromState(pivotTableState));
    }

    if (selection !== null && selection.kind === 'AREA' &&
        (pivotTableState.selectionArea === undefined ||
         pivotTableState.selectionArea.areaId !== selection.areaId)) {
      globals.dispatch(Actions.togglePivotTable({areaId: selection.areaId}));
    }
  }
}
