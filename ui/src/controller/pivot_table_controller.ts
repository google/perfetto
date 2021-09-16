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
  AggregationAttrs,
  AVAILABLE_AGGREGATIONS,
  AVAILABLE_TABLES,
  getDescendantsTables,
  getParentStackColumn,
  getStackColumn,
  getStackDepthColumn,
  PivotAttrs,
  PivotTableQueryResponse,
  removeHiddenAndAddStackColumns,
  RowAttrs,
  WHERE_FILTERS
} from '../common/pivot_table_common';
import {
  getAggregationAlias,
  getAggregationOverStackAlias,
  getAliasedPivotColumns,
  getHiddenPivotAlias,
  getPivotAlias,
  getTotalAggregationAlias,
  PivotTableQueryGenerator
} from '../common/pivot_table_query_generator';
import {
  QueryResponse,
  runQuery,
} from '../common/queries';
import {Row} from '../common/query_result';
import {toNs} from '../common/time';
import {PivotTableHelper} from '../frontend/pivot_table_helper';
import {publishPivotTableHelper, publishQueryResult} from '../frontend/publish';

import {Controller} from './controller';
import {globals} from './globals';

export interface PivotTableControllerArgs {
  pivotTableId: string;
  engine: Engine;
}

function getExpandableColumn(
    pivotTableId: string, queriedPivots: PivotAttrs[]): string|undefined {
  if (queriedPivots.length === 0) {
    return undefined;
  }
  const selectedPivots = globals.state.pivotTable[pivotTableId].selectedPivots;
  const lastPivot = getPivotAlias(selectedPivots[selectedPivots.length - 1]);
  const lastQueriedPivot =
      getPivotAlias(queriedPivots[queriedPivots.length - 1]);
  if (lastQueriedPivot !== lastPivot) {
    return lastQueriedPivot;
  }
  return undefined;
}

function getRowWhereFilters(
    queriedPivots: PivotAttrs[], row: Row, parentRow?: RowAttrs) {
  let whereFilters = new Map();

  // Add all the row's parent whereFilers.
  if (parentRow) {
    whereFilters = new Map(parentRow.whereFilters);
  }

  // Add whereFilters for all the queried pivots and any hidden pivots without
  // the stack pivots.
  getAliasedPivotColumns(queriedPivots)
      .filter(pivot => !pivot.pivotAttrs.isStackPivot)
      .forEach(
          pivot => whereFilters.set(
              pivot.columnAlias,
              `CAST(${pivot.pivotAttrs.tableName}.${
                  pivot.pivotAttrs.columnName} AS TEXT) = '${
                  row[pivot.columnAlias]!.toString()}'`));

  return whereFilters;
}

function getPivotTableQueryResponseRows(
    pivotTableId: string,
    rows: Row[],
    queriedPivots: PivotAttrs[],
    parentRow?: RowAttrs) {
  const expandableColumns = new Set<string>();
  if (queriedPivots.length > 0 && queriedPivots[0].isStackPivot) {
    // Make the stack column expandable.
    expandableColumns.add(getPivotAlias(queriedPivots[0]));
  }
  // Add expandable column after the stack column if it exists.
  const expandableColumn = getExpandableColumn(pivotTableId, queriedPivots);
  if (expandableColumn !== undefined) {
    expandableColumns.add(expandableColumn);
  }

  const newRows: RowAttrs[] = [];
  for (const row of rows) {
    newRows.push({
      row,
      expandableColumns,
      depth: 0,
      whereFilters: getRowWhereFilters(queriedPivots, row, parentRow),
      expandedRows: new Map()
    });
  }
  return newRows;
}

function getPivotTableQueryResponse(
    pivotTableId: string,
    queryResp: QueryResponse,
    queriedPivots: PivotAttrs[],
    parentRow?: RowAttrs): PivotTableQueryResponse {
  const columns = [];
  const pivotTable = globals.state.pivotTable[pivotTableId];

  for (let i = 0; i < pivotTable.selectedPivots.length; ++i) {
    const pivot = pivotTable.selectedPivots[i];
    columns.push({
      name: getPivotAlias(pivot),
      index: i,
      tableName: pivot.tableName,
      columnName: pivot.columnName,
      isStackColumn: pivot.isStackPivot
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
      isStackColumn: false
    });
  }

  return {
    columns,
    rows: getPivotTableQueryResponseRows(
        pivotTableId, queryResp.rows, queriedPivots, parentRow),
    error: queryResp.error,
    durationMs: queryResp.durationMs
  };
}

function getRowInPivotTableQueryResponse(
    queryResp: PivotTableQueryResponse,
    rowIndices: number[],
    expandedRowColumns: string[]) {
  if (rowIndices.length === 0) {
    throw new Error('Row indicies should have at least one index.');
  }

  let row = queryResp.rows[rowIndices[0]];

  // expandedRowColumns and rowIndices should refer to the same rows minus the
  // initial row index that specifies the row in the query response.
  if (rowIndices.length !== expandedRowColumns.length + 1) {
    throw Error(`expandedRowColumns length "${
        expandedRowColumns.length}" should be less than rowIndicies length "${
        rowIndices.length}" by one.`);
  }

  for (let i = 1; i < rowIndices.length; ++i) {
    const expandedRow = row.expandedRows.get(expandedRowColumns[i - 1]);
    if (expandedRow === undefined || expandedRow.rows.length <= rowIndices[i]) {
      throw new Error(`Expanded row index "${rowIndices[i]}" at row column "${
          expandedRowColumns[i - 1]}" is out of bounds.`);
    }
    row = expandedRow.rows[rowIndices[i]];
  }
  return row;
}

function getDescendantRows(
    pivotTableId: string,
    respRows: Row[],
    parentRow: RowAttrs,
    queriedPivots: PivotAttrs[],
    queriedAggregations: AggregationAttrs[]) {
  const stackPivot = queriedPivots[0];
  if (stackPivot === undefined || !stackPivot.isStackPivot) {
    throw Error('Queried pivot is not a stack pivot');
  }

  const stackIdColumn = getHiddenPivotAlias(getStackColumn(stackPivot));

  const parentStackIdColumn =
      getHiddenPivotAlias(getParentStackColumn(stackPivot));

  const depthColumn = getHiddenPivotAlias(getStackDepthColumn(stackPivot));

  const stackColumn = getPivotAlias(stackPivot);  // "name (stack)" column.

  const parentDepth = Number(parentRow.row[depthColumn]?.toString());
  if (!Number.isInteger(parentDepth)) {
    throw Error('Parent row has undefined depth.');
  }

  const parentRowStackId = parentRow.row[stackIdColumn]?.toString();
  if (parentRowStackId === undefined) {
    throw Error('Parent row has undefined stack_id.');
  }

  const nextPivot = queriedPivots[1];
  let nextColumnName = '';
  if (nextPivot !== undefined) {
    nextColumnName = getPivotAlias(nextPivot);
  }

  const newRows: Map<string, RowAttrs> = new Map();
  for (const row of respRows) {
    const depth = Number(row[depthColumn]?.toString());
    const stackId = row[stackIdColumn]?.toString();
    const parentStackId = row[parentStackIdColumn]?.toString();
    if (!Number.isInteger(depth)) {
      throw Error('Descendant result has undefined depth.');
    }
    if (!stackId || !parentStackId) {
      throw Error('Descendant result has undefined stack or parent stack id.');
    }

    const expandableColumns = new Set<string>();
    // Get expandable column after the stack column if it exists.
    const expandableColumn = getExpandableColumn(pivotTableId, queriedPivots);
    if (expandableColumn !== undefined) {
      expandableColumns.add(expandableColumn);
    }

    const newRow: RowAttrs = {
      row: Object.assign({}, row),
      depth: depth - parentDepth,
      whereFilters: getRowWhereFilters(queriedPivots, row, parentRow),
      expandedRows: new Map(),
      expandableColumns
    };

    // If we have already added the stackId, we need to extract and nest its
    // next column values in its expanded rows.
    if (newRows.has(stackId)) {
      newRow.row[stackColumn] = null;
      const parent = newRows.get(stackId)!;
      let nextColumnRows = parent.expandedRows.get(nextColumnName);
      if (nextColumnRows === undefined) {
        // Since the parent row has more than one value for the next column,
        // we nest the values in rows under the parent row instead of inline
        // with it.
        // Making a new row to hold the next column value.
        const row = Object.assign({}, parent.row);
        const nextColumnRow: RowAttrs = {
          row: Object.assign({}, row),
          depth: depth - parentDepth,
          whereFilters: getRowWhereFilters(queriedPivots, row, parentRow),
          expandedRows: new Map(),
          expandableColumns
        };
        parent.row[nextColumnName] = null;
        // Modify the parent row to show the aggregation over stack rows instead
        // of the partitioned aggregations.
        for (const aggregation of queriedAggregations) {
          parent.row[getAggregationAlias(aggregation)] =
              parent.row[getAggregationOverStackAlias(aggregation)];
        }
        nextColumnRow.row[stackColumn] = null;
        if (nextColumnRow.row[nextColumnName] !== undefined) {
          parent.expandedRows.set(
              nextColumnName, {isExpanded: true, rows: [nextColumnRow]});
        }
        nextColumnRows = parent.expandedRows.get(nextColumnName);
      }
      newRow.expandableColumns = expandableColumns;

      nextColumnRows!.rows.push(newRow);
    }

    // If we have already added the parentStackId, we need to nest the row
    // in its parent's expanded rows.
    // This works because we sort the result of the descendants query by
    // depth, insuring that if the stack_id has a parent other than the
    // parent row, its parent will show up first.
    if (newRows.has(parentStackId)) {
      const parent = newRows.get(parentStackId)!;
      let descendants = parent.expandedRows.get(stackColumn);
      if (descendants === undefined) {
        parent.expandedRows.set(stackColumn, {isExpanded: true, rows: []});
        descendants = parent.expandedRows.get(stackColumn);
      }
      descendants!.rows.push(newRow);
      parent.expandableColumns.add(stackColumn);

      // Unexpand if parent has more than one child.
      if (descendants!.rows.length > 1) {
        descendants!.isExpanded = false;
        if (parent.expandedRows.has(nextColumnName)) {
          parent.expandedRows.get(nextColumnName)!.isExpanded = false;
        }
      }
    }

    if (!newRows.has(stackId)) {
      newRows.set(stackId, newRow);
    }
  }

  // Get only direct descendants of the parent row. The rest is nested inside
  // the descendants.
  const descendantRows =
      Array.from(newRows.values())
          .filter(
              row => row.row[parentStackIdColumn]?.toString() ===
                  parentRowStackId);
  // Get the next column values of the parent row.
  let nextColumnRows;
  if (newRows.has(parentRowStackId)) {
    if (newRows.get(parentRowStackId)!.expandedRows.has(nextColumnName)) {
      nextColumnRows =
          newRows.get(parentRowStackId)!.expandedRows.get(nextColumnName)!.rows;
    } else {
      // If the next column value is inline with the parent row.
      nextColumnRows = [newRows.get(parentRowStackId)!];
    }
  }
  return {descendantRows, nextColumnRows};
}

function getPivotColumns(
    pivotTableId: string, columnIdx: number, isStackQuery: boolean) {
  const selectedPivots = globals.state.pivotTable[pivotTableId].selectedPivots;
  // Slice returns the pivot at columnIdx if it exists, and an empty
  // array if columnIdx is out of bounds.
  const pivots = selectedPivots.slice(columnIdx, columnIdx + 1);
  if (isStackQuery) {
    // Adds the next pivot, if it exists, to be queried with the stack query.
    pivots.push(...selectedPivots.slice(columnIdx + 1, columnIdx + 2));
  }
  return pivots;
}

function getWhereFilters(
    pivotTableId: string,
    parentRowWhereFilters: Map<string, string>,
    pivots: PivotAttrs[],
    isStackQuery: boolean) {
  const whereFiltersMap = new Map(parentRowWhereFilters);

  // Remove any existing where filters for the pivots to query.
  getAliasedPivotColumns(pivots).forEach(
      pivotAlias => whereFiltersMap.delete(pivotAlias.columnAlias));

  const whereFilters = Array.from(whereFiltersMap.values());

  if (pivots.length > 0 && pivots[0].isStackPivot && !isStackQuery) {
    // Only query top level slices, descendants can be generated
    // when expanded.
    const orderColumn = getStackDepthColumn(pivots[0]);
    whereFilters.push(`${orderColumn.tableName}.${orderColumn.columnName} = 0`);
  }

  // Add global where filters
  whereFilters.push(...WHERE_FILTERS);

  // Add area restrictions where filters if set.
  const pivotTable = globals.state.pivotTable[pivotTableId];
  if (pivotTable.selectedTrackIds) {
    whereFilters.push(`slice.track_id IN (${pivotTable.selectedTrackIds})`);
  }
  if (pivotTable.traceTime) {
    whereFilters.push(
        `slice.ts + slice.dur > ${toNs(pivotTable.traceTime.startSec)}`);
    whereFilters.push(`slice.ts < ${toNs(pivotTable.traceTime.endSec)}`);
  }

  return whereFilters;
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
      case 'DESCENDANTS':
        const descendantsAttrs = requestedAction.attrs;
        if (descendantsAttrs === undefined) {
          throw Error('No attributes provided for descendants query.');
        }
        if (this.queryResp === undefined) {
          throw Error(
              'Descendants query requested without setting the main query.');
        }

        const stackPivot =
            pivotTable.selectedPivots[descendantsAttrs.columnIdx];
        const stackColumnName = getPivotAlias(stackPivot);

        const nextPivot =
            pivotTable.selectedPivots[descendantsAttrs.columnIdx + 1];
        let nextColumnName = '';
        if (nextPivot !== undefined) {
          nextColumnName = getPivotAlias(nextPivot);
        }

        const ancestorRow = getRowInPivotTableQueryResponse(
            this.queryResp,
            descendantsAttrs.rowIndices,
            descendantsAttrs.expandedRowColumns);

        // No need to query if the row has been expanded before.
        if (ancestorRow.expandedRows.has(stackColumnName)) {
          ancestorRow.expandedRows.get(stackColumnName)!.isExpanded = true;
          if (ancestorRow.expandedRows.has(nextColumnName) &&
              !ancestorRow.expandableColumns.has(nextColumnName)) {
            ancestorRow.expandedRows.get(nextColumnName)!.isExpanded = true;
          }
          break;
        }

        const descendantsPivots = getPivotColumns(
            this.pivotTableId,
            descendantsAttrs.columnIdx,
            /* is_stack_query = */ true);

        if (descendantsPivots.length === 0) {
          throw Error(
              `Descendant operation at column index "${
                  descendantsAttrs
                      .columnIdx}" should only be allowed if there are` +
              `are more columns to query.`);
        }

        const descendantsWhereFilters = getWhereFilters(
            this.pivotTableId,
            ancestorRow.whereFilters,
            descendantsPivots,
            /* is_stack_query = */ true);

        const stackIdColumn = getHiddenPivotAlias(getStackColumn(stackPivot));

        const stackId = ancestorRow.row[stackIdColumn]?.toString();
        if (stackId === undefined) {
          throw Error(`"${
              getPivotAlias(
                  stackPivot)}" row has undefined stack id at column "${
              stackIdColumn}".`);
        }

        const descendantsTables =
            getDescendantsTables(descendantsPivots, stackId);

        // Query the descendants and the next column if it exists.
        const descendantsQuery =
            this.pivotTableQueryGenerator.generateStackQuery(
                descendantsPivots,
                pivotTable.selectedAggregations,
                descendantsWhereFilters,
                descendantsTables,
                stackId);

        ancestorRow.loadingColumn = stackColumnName;

        runQuery(this.pivotTableId, descendantsQuery, this.engine)
            .then(resp => {
              // Query resulting from query generator should always be valid.
              if (resp.error) {
                throw Error(`Pivot table descendants query ${
                    descendantsQuery} resulted in SQL error: ${resp.error}`);
              }

              const printDescendantsQuery =
                  descendantsQuery.length <= 1024 ? descendantsQuery : '';
              console.log(`Descendants query${printDescendantsQuery} took ${
                  resp.durationMs} ms`);

              const {descendantRows, nextColumnRows} = getDescendantRows(
                  this.pivotTableId,
                  resp.rows,
                  ancestorRow,
                  descendantsPivots,
                  pivotTable.selectedAggregations);
              // Set descendant rows.
              ancestorRow.expandedRows.set(
                  stackColumnName, {isExpanded: true, rows: descendantRows});
              // Set the next pivot row(s), if they exist, inside the parent
              // row.
              if (nextColumnRows !== undefined) {
                if (nextColumnRows.length <= 1) {
                  // If there is only one value for the next column of the
                  // parent row, add it to the same row as the parent.
                  ancestorRow.row = nextColumnRows[0].row;
                  ancestorRow.expandableColumns = new Set([
                    ...nextColumnRows[0].expandableColumns,
                    ...ancestorRow.expandableColumns
                  ]);
                } else {
                  ancestorRow.expandedRows.set(
                      nextColumnName, {isExpanded: true, rows: nextColumnRows});
                }
              }
              ancestorRow.loadingColumn = undefined;

              this.queryResp!.durationMs += resp.durationMs;
            });
        break;

      case 'EXPAND':
        const expandAttrs = requestedAction.attrs;
        if (expandAttrs === undefined) {
          throw Error('No attributes provided for expand query.');
        }
        if (this.queryResp === undefined) {
          throw Error('Expand query requested without setting the main query.');
        }

        const expandColumnName =
            getPivotAlias(pivotTable.selectedPivots[expandAttrs.columnIdx]);

        const expandRow = getRowInPivotTableQueryResponse(
            this.queryResp,
            expandAttrs.rowIndices,
            expandAttrs.expandedRowColumns);

        // No need to query if the row has been expanded before.
        if (expandRow.expandedRows.has(expandColumnName)) {
          expandRow.expandedRows.get(expandColumnName)!.isExpanded = true;
          break;
        }

        const expandPivots = getPivotColumns(
            this.pivotTableId,
            expandAttrs.columnIdx + 1,
            /* is_stack_query = */ false);

        if (expandPivots.length === 0) {
          throw Error(
              `Expand operation at column index "${
                  expandAttrs.columnIdx}" should only be allowed if there are` +
              `are more columns to query.`);
        }

        const expandWhereFilters = getWhereFilters(
            this.pivotTableId,
            expandRow.whereFilters,
            expandPivots,
            /* is_stack_query = */ false);

        const expandQuery = this.pivotTableQueryGenerator.generateQuery(
            expandPivots,
            pivotTable.selectedAggregations,
            expandWhereFilters,
            AVAILABLE_TABLES);

        expandRow.loadingColumn =
            getPivotAlias(pivotTable.selectedPivots[expandAttrs.columnIdx]);

        runQuery(this.pivotTableId, expandQuery, this.engine).then(resp => {
          // Query resulting from query generator should always be valid.
          if (resp.error) {
            throw Error(`Pivot table expand query ${
                expandQuery} resulted in SQL error: ${resp.error}`);
          }
          const printExpandQuery =
              expandQuery.length <= 1024 ? expandQuery : '';
          console.log(
              `Expand query${printExpandQuery} took ${resp.durationMs} ms`);

          expandRow.expandedRows.set(expandColumnName, {
            isExpanded: true,
            rows: getPivotTableQueryResponseRows(
                this.pivotTableId, resp.rows, expandPivots, expandRow)
          });
          expandRow.loadingColumn = undefined;

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

        const unexpandPivot =
            pivotTable.selectedPivots[unexpandAttrs.columnIdx];
        const unexpandColumnName = getPivotAlias(unexpandPivot);

        const unexpandRow = getRowInPivotTableQueryResponse(
            this.queryResp,
            unexpandAttrs.rowIndices,
            unexpandAttrs.expandedRowColumns);

        if (unexpandRow.expandedRows.has(unexpandColumnName)) {
          unexpandRow.expandedRows.get(unexpandColumnName)!.isExpanded = false;
          const nextPivot =
              pivotTable.selectedPivots[unexpandAttrs.columnIdx + 1];
          let nextColumnName = '';
          if (nextPivot !== undefined) {
            nextColumnName = getPivotAlias(nextPivot);
          }
          // Unexpand the next column rows if they are nested inside the
          // parent expanded row, but no expandable column exists for the
          // next column.
          if (unexpandPivot.isStackPivot &&
              unexpandRow.expandedRows.has(nextColumnName) &&
              !unexpandRow.expandableColumns.has(nextColumnName)) {
            unexpandRow.expandedRows.get(nextColumnName)!.isExpanded = false;
          }
        } else {
          throw Error('Unexpand request called on already undexpanded row.');
        }
        break;

      case 'QUERY':
        // Generates and executes new query based on selectedPivots and
        // selectedAggregations.

        const pivots = getPivotColumns(
            this.pivotTableId,
            /* column_idx = */ 0,
            /* is_stack_query = */ false);

        const whereFilers = getWhereFilters(
            this.pivotTableId,
            /*parent_row_where_Filters = */ new Map(),
            pivots,
            /* is_stack_query = */ false);

        const query = this.pivotTableQueryGenerator.generateQuery(
            pivots,
            pivotTable.selectedAggregations,
            whereFilers,
            AVAILABLE_TABLES);

        if (query !== '') {
          globals.dispatch(
              Actions.toggleQueryLoading({pivotTableId: this.pivotTableId}));
          runQuery(this.pivotTableId, query, this.engine).then(resp => {
            // Query resulting from query generator should always be valid.
            if (resp.error) {
              throw Error(`Pivot table query ${query} resulted in SQL error: ${
                  resp.error}`);
            }

            const printQuery = query.length <= 1024 ? query : '';
            console.log(`Query${printQuery} took ${resp.durationMs} ms`);
            const data =
                getPivotTableQueryResponse(this.pivotTableId, resp, pivots);

            if (pivotTable.selectedAggregations.length > 0 &&
                resp.rows.length > 0) {
              const totalAggregationsRow = Object.assign({}, resp.rows[0]);

              // Modify the total aggregations row to show the total
              // aggregations.
              if (pivotTable.selectedPivots.length > 0) {
                for (const aggregation of pivotTable.selectedAggregations) {
                  totalAggregationsRow[getAggregationAlias(aggregation)] =
                      totalAggregationsRow[getTotalAggregationAlias(
                          aggregation)];
                }
              }
              data.totalAggregations = totalAggregationsRow;
            }

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
    return removeHiddenAndAddStackColumns(tableName, resp.columns);
  }
}
