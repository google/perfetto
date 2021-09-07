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

import {Actions} from '../common/actions';
import {Engine} from '../common/engine';
import {
  AVAILABLE_AGGREGATIONS,
  AVAILABLE_TABLES,
  PivotTableQueryResponse,
} from '../common/pivot_table_data';
import {
  getAggregationAlias,
  getPivotAlias,
  PivotTableQueryGenerator
} from '../common/pivot_table_query_generator';
import {
  QueryResponse,
  runQuery,
} from '../common/queries';
import {PivotTableHelper} from '../frontend/pivot_table_helper';
import {publishPivotTableHelper, publishQueryResult} from '../frontend/publish';

import {Controller} from './controller';
import {globals} from './globals';

export interface PivotTableControllerArgs {
  pivotTableId: string;
  engine: Engine;
}

function getPivotTableQueryResponse(
    pivotTableId: string, queryResp: QueryResponse): PivotTableQueryResponse {
  const columns = [];
  const pivotTable = globals.state.pivotTable[pivotTableId];
  for (const column of queryResp.columns) {
    let isPivot = false;
    let index = pivotTable.selectedAggregations.findIndex(
        element => column.startsWith(getAggregationAlias(element)));
    if (index === -1) {
      isPivot = true;
      index = pivotTable.selectedPivots.findIndex(
          element => column.startsWith(getPivotAlias(element)));
    }
    if (index === -1) {
      throw Error(
          'Column in query response not in selectedAggregations or ' +
          'selectedPivots.');
    }
    let tableName;
    let columnName;
    let aggregation;
    let order;
    if (isPivot) {
      tableName = pivotTable.selectedPivots[index].tableName;
      columnName = pivotTable.selectedPivots[index].columnName;
    } else {
      tableName = pivotTable.selectedAggregations[index].tableName;
      columnName = pivotTable.selectedAggregations[index].columnName;
      aggregation = pivotTable.selectedAggregations[index].aggregation;
      order = pivotTable.selectedAggregations[index].order;
    }
    columns.push(
        {name: column, index, tableName, columnName, aggregation, order});
  }

  return {
    columns,
    rows: queryResp.rows,
    error: queryResp.error,
    durationMs: queryResp.durationMs
  };
}

export class PivotTableController extends Controller<'main'> {
  private pivotTableId: string;
  private pivotTableQueryGenerator = new PivotTableQueryGenerator();
  private engine: Engine;
  private previousQuery = '';

  constructor(args: PivotTableControllerArgs) {
    super('main');
    this.engine = args.engine;
    this.pivotTableId = args.pivotTableId;
    this.setup().then(() => {
      this.run();
    });
  }

  run() {
    const {requestedAction} = globals.state.pivotTable[this.pivotTableId];
    if (!requestedAction) return;
    globals.dispatch(
        Actions.resetPivotTableRequest({pivotTableId: this.pivotTableId}));
    switch (requestedAction) {
      case 'QUERY':
        // Generates and executes new query based on selectedPivots and
        // selectedAggregations.
        const pivotTable = globals.state.pivotTable[this.pivotTableId];
        const query = this.pivotTableQueryGenerator.generateQuery(
            pivotTable.selectedPivots, pivotTable.selectedAggregations);
        if (query === this.previousQuery) break;
        if (query !== '') {
          globals.dispatch(
              Actions.toggleQueryLoading({pivotTableId: this.pivotTableId}));
          runQuery(this.pivotTableId, query, this.engine).then(resp => {
            console.log(`Query ${query} took ${resp.durationMs} ms`);
            publishQueryResult({
              id: this.pivotTableId,
              data: getPivotTableQueryResponse(this.pivotTableId, resp)
            });
            globals.dispatch(
                Actions.toggleQueryLoading({pivotTableId: this.pivotTableId}));
          });
        } else {
          publishQueryResult({id: this.pivotTableId, data: undefined});
        }
        this.previousQuery = query;
        break;

      default:
        throw new Error(`Unexpected requested action ${requestedAction}`);
    }
  }

  private async setup(): Promise<void> {
    const pivotTable = globals.state.pivotTable[this.pivotTableId];
    const selectedPivots = pivotTable.selectedPivots;
    const selectedAggregations = pivotTable.selectedAggregations;
    let availableColumns = globals.state.pivotTableConfig.availableColumns;
    // No need to retrieve table columns if they are already stored.
    // Only needed when first pivot table is created.
    if (availableColumns === undefined) {
      availableColumns = [];
      for (const table of AVAILABLE_TABLES) {
        const columns = await this.getColumnsForTable(table);
        if (columns.length > 0) {
          availableColumns.push({tableName: table, columns});
        }
      }
      globals.dispatch(Actions.setAvailablePivotTableColumns(
          {availableColumns, availableAggregations: AVAILABLE_AGGREGATIONS}));
    }
    publishPivotTableHelper({
      id: this.pivotTableId,
      data: new PivotTableHelper(
          this.pivotTableId,
          availableColumns,
          AVAILABLE_AGGREGATIONS,
          selectedPivots,
          selectedAggregations)
    });
  }

  private async getColumnsForTable(tableName: string): Promise<string[]> {
    const query = `select * from ${tableName} limit 0;`;
    const resp = await runQuery(this.pivotTableId, query, this.engine);
    return resp.columns;
  }
}
