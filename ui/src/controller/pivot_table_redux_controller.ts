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
import {Engine} from '../common/engine';
import {featureFlags} from '../common/feature_flags';
import {ColumnType} from '../common/query_result';
import {
  PivotTableReduxQueryMetadata,
  PivotTableReduxResult
} from '../common/state';
import {aggregationIndex} from '../frontend/pivot_table_redux_query_generator';

import {Controller} from './controller';
import {globals} from './globals';

export const PIVOT_TABLE_REDUX_FLAG = featureFlags.register({
  id: 'pivotTableRedux',
  name: 'Pivot tables V2',
  description: 'Second version of pivot table',
  defaultValue: false,
});

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

// Auxiliary class to build the tree from query response.
class TreeBuilder {
  private readonly root: PivotTree;
  lastRow: ColumnType[];
  pivotColumns: number;
  aggregateColumns: number;

  constructor(
      pivotColumns: number, aggregateColumns: number, firstRow: ColumnType[]) {
    this.pivotColumns = pivotColumns;
    this.aggregateColumns = aggregateColumns;
    this.root = this.createNode(0, firstRow);
    let tree = this.root;
    for (let i = 0; i + 1 < this.pivotColumns; i++) {
      const value = firstRow[i];
      tree = TreeBuilder.insertChild(
          tree, value, this.createNode(i + 1, firstRow));
    }
    this.lastRow = firstRow;
  }

  // Add incoming row to the tree being built.
  ingestRow(row: ColumnType[]) {
    let tree = this.root;
    for (let i = 0; i + 1 < this.pivotColumns; i++) {
      const nextTree = tree.children.get(row[i]);
      if (nextTree === undefined) {
        // Insert the new node into the tree, and make variable `tree` point
        // to the newly created node.
        tree =
            TreeBuilder.insertChild(tree, row[i], this.createNode(i + 1, row));
      } else {
        tree = nextTree;
      }
    }
    tree.rows.push(row);
    this.lastRow = row;
  }

  build(): PivotTree {
    return this.root;
  }

  // Helper method that inserts child node into the tree and returns it, used
  // for more concise modification of local variable pointing to the current
  // node being built.
  static insertChild(tree: PivotTree, key: ColumnType, child: PivotTree):
      PivotTree {
    tree.children.set(key, child);
    return child;
  }

  // Initialize PivotTree from a row.
  createNode(depth: number, row: ColumnType[]): PivotTree {
    const aggregates = [];

    for (let j = 0; j < this.aggregateColumns; j++) {
      aggregates.push(row[aggregationIndex(this.pivotColumns, j, depth)]);
    }

    return {
      isCollapsed: false,
      children: new Map(),
      aggregates,
      rows: [],
    };
  }
}

function createEmptyQueryResult(metadata: PivotTableReduxQueryMetadata):
    PivotTableReduxResult {
  return {
    tree: {
      aggregates: [],
      isCollapsed: false,
      children: new Map(),
      rows: [],
    },
    metadata
  };
}


// Controller responsible for showing the panel with pivot table, as well as
// executing its queries and post-processing query results.
export class PivotTableReduxController extends Controller<{}> {
  engine: Engine;
  lastStartedQueryId: number;

  constructor(args: {engine: Engine}) {
    super({});
    this.engine = args.engine;
    this.lastStartedQueryId = 0;
  }

  run() {
    if (!PIVOT_TABLE_REDUX_FLAG.get()) {
      return;
    }

    const pivotTableState = globals.state.pivotTableRedux;
    if (pivotTableState.queryId > this.lastStartedQueryId &&
        pivotTableState.query !== null) {
      this.lastStartedQueryId = pivotTableState.queryId;
      const query = pivotTableState.query;

      this.engine.query(query.text).then(async (result) => {
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
          globals.dispatch(Actions.setPivotStateReduxState({
            pivotTableState: {
              queryId: this.lastStartedQueryId,
              query: null,
              queryResult: createEmptyQueryResult(query.metadata),
              selectionArea: pivotTableState.selectionArea
            }
          }));
          return;
        }

        const treeBuilder = new TreeBuilder(
            query.metadata.pivotColumns.length,
            query.metadata.aggregationColumns.length,
            nextRow());
        while (it.valid()) {
          treeBuilder.ingestRow(nextRow());
        }

        globals.dispatch(Actions.setPivotStateReduxState({
          pivotTableState: {
            queryId: this.lastStartedQueryId,
            query: null,
            queryResult: {
              tree: treeBuilder.build(),
              metadata: query.metadata,
            },
            selectionArea: pivotTableState.selectionArea
          }
        }));
      });
    }

    const selection = globals.state.currentSelection;
    if (selection !== null && selection.kind === 'AREA') {
      const enabledArea = globals.state.areas[selection.areaId];
      globals.dispatch(
          Actions.togglePivotTableRedux({selectionArea: enabledArea}));
    } else {
      globals.dispatch(Actions.togglePivotTableRedux({selectionArea: null}));
    }
  }
}