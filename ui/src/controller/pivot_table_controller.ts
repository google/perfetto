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
  RowAttrs,
  WHERE_FILTERS
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
import {Row} from '../common/query_result';
import {PivotTableHelper} from '../frontend/pivot_table_helper';
import {publishPivotTableHelper, publishQueryResult} from '../frontend/publish';

import {Controller} from './controller';
import {globals} from './globals';

export interface PivotTableControllerArgs {
  pivotTableId: string;
  engine: Engine;
}

function getExpandableColumn(pivotTableId: string, columns: string[]): string|
    undefined {
  const pivotTable = globals.state.pivotTable[pivotTableId];
  const lastQueriedPivotIdx =
      columns.length - pivotTable.selectedAggregations.length - 1;
  if (lastQueriedPivotIdx < 0) {
    return undefined;
  }
  const selectedPivots = pivotTable.selectedPivots;
  const lastPivot = getPivotAlias(selectedPivots[selectedPivots.length - 1]);
  if (columns[lastQueriedPivotIdx] !== lastPivot) {
    return columns[lastQueriedPivotIdx];
  }
  return undefined;
}

function getPivotTableQueryResponseRows(
    pivotTableId: string, rows: Row[], columns: string[]): RowAttrs[] {
  const expandableColumn = getExpandableColumn(pivotTableId, columns);
  const newRows: RowAttrs[] = [];
  for (const row of rows) {
    newRows.push({
      row,
      isExpanded: false,
      expandableColumn,
      rows: undefined,
      isLoadingQuery: false
    });
  }
  return newRows;
}

function getPivotTableQueryResponse(
    pivotTableId: string, queryResp: QueryResponse): PivotTableQueryResponse {
  const columns = [];
  const pivotTable = globals.state.pivotTable[pivotTableId];

  for (let i = 0; i < pivotTable.selectedPivots.length; ++i) {
    const pivot = pivotTable.selectedPivots[i];
    columns.push({
      name: getPivotAlias(pivot),
      index: i,
      tableName: pivot.tableName,
      columnName: pivot.columnName,
    });
  }

  for (let i = 0; i < pivotTable.selectedAggregations.length; ++i) {
    const aggregation = pivotTable.selectedAggregations[i];
    columns.push({
      name: getAggregationAlias(aggregation),
      index: i,
      tableName: aggregation.tableName,
      columnName: aggregation.columnName,
      aggregation: aggregation.aggregation,
      order: aggregation.order,
    });
  }

  return {
    columns,
    rows: getPivotTableQueryResponseRows(
        pivotTableId, queryResp.rows, queryResp.columns),
    error: queryResp.error,
    durationMs: queryResp.durationMs
  };
}

function getRowAndWhereFiltersInPivotTableQueryResponse(
    queryResp: PivotTableQueryResponse, rowIndices: number[]) {
  if (rowIndices.length === 0) {
    throw new Error('Row indicies should have at least one index.');
  }
  let row = queryResp.rows[rowIndices[0]];
  const whereFilters = [];
  for (let i = 1; i < rowIndices.length; ++i) {
    if (row.whereFilter !== undefined) {
      whereFilters.push(row.whereFilter);
    }
    if (row.rows === undefined || row.rows.length <= rowIndices[i]) {
      throw new Error(
          `Expanded row index "${rowIndices[i]}" is out of bounds.`);
    }
    row = row.rows[rowIndices[i]];
  }
  return {row, whereFilters};
}

export class PivotTableController extends Controller<'main'> {
  private pivotTableId: string;
  private pivotTableQueryGenerator = new PivotTableQueryGenerator();
  private engine: Engine;
  private queryResp?: PivotTableQueryResponse;

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
    const pivotTable = globals.state.pivotTable[this.pivotTableId];
    if (!requestedAction) return;
    globals.dispatch(
        Actions.resetPivotTableRequest({pivotTableId: this.pivotTableId}));
    switch (requestedAction.action) {
      case 'EXPAND':
        const expandAttrs = requestedAction.attrs;
        if (expandAttrs === undefined) {
          throw Error('No attributes provided for expand query.');
        }
        if (this.queryResp === undefined) {
          throw Error('Expand query requested without setting the main query.');
        }

        const {row: expandRow, whereFilters} =
            getRowAndWhereFiltersInPivotTableQueryResponse(
                this.queryResp, expandAttrs.rowIndices);

        // No need to query if the row has been expanded before.
        if (expandRow.rows !== undefined) {
          expandRow.isExpanded = true;
          publishQueryResult({id: this.pivotTableId, data: this.queryResp});
          break;
        }

        const whereFilter = `CAST(${
            pivotTable.selectedPivots[expandAttrs.columnIdx].tableName}.${
            pivotTable.selectedPivots[expandAttrs.columnIdx]
                .columnName} AS TEXT) = '${expandAttrs.value}'`;

        whereFilters.push(whereFilter);
        whereFilters.push(...WHERE_FILTERS);

        // Slice returns an empty array if indexes are out of bounds.
        const pivots = pivotTable.selectedPivots.slice(
            expandAttrs.columnIdx + 1, expandAttrs.columnIdx + 2);

        if (pivots.length === 0) {
          throw Error(
              `Expand operation at column index "${
                  expandAttrs.columnIdx}" should only be allowed if there are` +
              `are more columns to query.`);
        }

        // Query the column after the expanded column.
        const expandQuery = this.pivotTableQueryGenerator.generateQuery(
            pivots, pivotTable.selectedAggregations, whereFilters);

        expandRow.isLoadingQuery = true;

        runQuery(this.pivotTableId, expandQuery, this.engine).then(resp => {
          // Query resulting from query generator should always be valid.
          if (resp.error) {
            throw Error(`Pivot table expand query ${
                expandQuery} resulted in SQL error: ${resp.error}`);
          }
          console.log(`Expand query ${expandQuery} took ${resp.durationMs} ms`);

          expandRow.rows = getPivotTableQueryResponseRows(
              this.pivotTableId, resp.rows, resp.columns);
          expandRow.isExpanded = true;
          expandRow.whereFilter = whereFilter;
          expandRow.isLoadingQuery = false;

          this.queryResp!.durationMs += resp.durationMs;
        });
        break;

      case 'UNEXPAND':
        const unexpandAttrs = requestedAction.attrs;
        if (unexpandAttrs === undefined) {
          throw Error('No attributes provided for unexpand query.');
        }
        if (this.queryResp === undefined) {
          throw Error(
              'Unexpand query requested without setting the main query.');
        }

        const {row: unexpandRow} =
            getRowAndWhereFiltersInPivotTableQueryResponse(
                this.queryResp, unexpandAttrs.rowIndices);

        unexpandRow.isExpanded = false;
        break;

      case 'QUERY':
        // Generates and executes new query based on selectedPivots and
        // selectedAggregations.
        // Query the first column.
        const query = this.pivotTableQueryGenerator.generateQuery(
            pivotTable.selectedPivots.slice(0, 1),
            pivotTable.selectedAggregations,
            WHERE_FILTERS);
        if (query !== '') {
          globals.dispatch(
              Actions.toggleQueryLoading({pivotTableId: this.pivotTableId}));
          runQuery(this.pivotTableId, query, this.engine).then(resp => {
            // Query resulting from query generator should always be valid.
            if (resp.error) {
              throw Error(`Pivot table query ${query} resulted in SQL error: ${
                  resp.error}`);
            }

            console.log(`Query ${query} took ${resp.durationMs} ms`);
            const data = getPivotTableQueryResponse(this.pivotTableId, resp);
            publishQueryResult({id: this.pivotTableId, data});

            this.queryResp = data;
            globals.dispatch(
                Actions.toggleQueryLoading({pivotTableId: this.pivotTableId}));
          });
        } else {
          publishQueryResult({id: this.pivotTableId, data: undefined});
        }
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
