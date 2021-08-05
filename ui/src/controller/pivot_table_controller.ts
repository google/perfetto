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
import {PivotTableQueryGenerator} from '../common/pivot_table_query_generator';
import {runQuery} from '../common/queries';
import {publishQueryResult} from '../frontend/publish';

import {Controller} from './controller';
import {globals} from './globals';

const AVAILABLE_TABLES = ['slice'];
const AVAILABLE_AGGREGATIONS = ['COUNT', 'SUM', 'AVG', 'MIN', 'MAX'];

export interface PivotTableControllerArgs {
  pivotTableId: string;
  engine: Engine;
}

export class PivotTableController extends Controller<'main'> {
  private pivotTableQueryGenerator = new PivotTableQueryGenerator();
  private engine: Engine;
  private pivotTableId: string;
  private availableColumns: Array<{tableName: string, columns: string[]}> = [];
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
      case 'UPDATE':
        globals.dispatch(Actions.toggleRequestedPivotTablePivot(
            {pivotTableId: this.pivotTableId}));
        break;

      case 'QUERY':
        // Generates and executes new query based on selectedPivots and
        // selectedAggregations.
        const pivotTable = globals.state.pivotTable[this.pivotTableId];
        const query = this.pivotTableQueryGenerator.generateQuery(
            pivotTable.selectedPivots, pivotTable.selectedAggregations);
        if (query === this.previousQuery) break;
        if (query !== '') {
          runQuery(this.pivotTableId, query, this.engine).then(resp => {
            console.log(`Query ${query} took ${resp.durationMs} ms`);
            publishQueryResult({id: this.pivotTableId, data: resp});
          });
        } else {
          publishQueryResult({id: this.pivotTableId, data: undefined});
        }
        this.previousQuery = query;
        break;

      default:
        throw new Error(`Unexpected state ${this.state}`);
    }
  }

  private async setup(): Promise<void> {
    // No need to retrieve table columns if they are already stored.
    // Only needed when first pivot table is created.
    if (globals.state.pivotTableConfig.availableColumns !== undefined) return;
    let totalColumnsCount = 0;
    for (const table of AVAILABLE_TABLES) {
      const columns = await this.getColumnsForTable(table);
      totalColumnsCount += columns.length;
      if (columns.length > 0) {
        this.availableColumns.push({tableName: table, columns});
      }
    }
    globals.dispatch(Actions.setAvailablePivotTableColumns({
      availableColumns: this.availableColumns,
      totalColumnsCount,
      availableAggregations: AVAILABLE_AGGREGATIONS
    }));
  }

  private async getColumnsForTable(tableName: string): Promise<string[]> {
    const query = `select * from ${tableName} limit 0;`;
    const resp = await runQuery(this.pivotTableId, query, this.engine);
    return resp.columns;
  }
}
