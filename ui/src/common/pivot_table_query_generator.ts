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

import {
  AggregationAttrs,
  AVAILABLE_TABLES,
  getHiddenStackHelperColumns,
  getParentStackWhereFilter,
  getStackColumn,
  getStackDepthColumn,
  PivotAttrs,
} from './pivot_table_common';

export function getPivotAlias(pivot: PivotAttrs): string {
  return `${pivot.tableName} ${pivot.columnName}`;
}

export function getHiddenPivotAlias(pivot: PivotAttrs) {
  return getPivotAlias(pivot) + ' (hidden)';
}

export function getAggregationAlias(aggregation: AggregationAttrs): string {
  return `${aggregation.tableName} ${aggregation.columnName} (${
      aggregation.aggregation})`;
}

export function getSqlPivotAlias(pivot: PivotAttrs): string {
  return `"${getPivotAlias(pivot)}"`;
}

export function getSqlHiddenPivotAlias(pivot: PivotAttrs): string {
  return `"${getHiddenPivotAlias(pivot)}"`;
}

export function getSqlAggregationAlias(aggregation: AggregationAttrs): string {
  return `"${getAggregationAlias(aggregation)}"`;
}

export function getAggregationOverStackAlias(aggregation: AggregationAttrs):
    string {
  return `${getAggregationAlias(aggregation)} (stack)`;
}

export function getSqlAggregationOverStackAlias(aggregation: AggregationAttrs):
    string {
  return `"${getAggregationOverStackAlias(aggregation)}"`;
}

export function getTotalAggregationAlias(aggregation: AggregationAttrs):
    string {
  return `${getAggregationAlias(aggregation)} (total)`;
}

export function getSqlTotalAggregationAlias(aggregation: AggregationAttrs):
    string {
  return `"${getTotalAggregationAlias(aggregation)}"`;
}

// Returns an array of pivot aliases along with any additional pivot aliases.
export function getSqlAliasedPivotColumns(pivots: PivotAttrs[]): string[] {
  const pivotCols = [];
  for (const pivot of pivots) {
    pivotCols.push(getSqlPivotAlias(pivot));
    if (pivot.isStackPivot) {
      pivotCols.push(...getHiddenStackHelperColumns(pivot).map(
          column => `"${column.columnAlias}"`));
    }
  }
  return pivotCols;
}

export function getAliasedPivotColumns(pivots: PivotAttrs[]) {
  const pivotCols: Array<{pivotAttrs: PivotAttrs, columnAlias: string}> = [];
  for (const pivot of pivots) {
    pivotCols.push({pivotAttrs: pivot, columnAlias: getPivotAlias(pivot)});
    if (pivot.isStackPivot) {
      pivotCols.push(...getHiddenStackHelperColumns(pivot));
    }
  }
  return pivotCols;
}

// Returns an array of aggregation aliases along with total aggregations if
// necessary.
function getSqlAliasedAggregationsColumns(
    aggregations: AggregationAttrs[],
    hasPivotsSelected: boolean,
    isStackQuery: boolean): string[] {
  const aggCols =
      aggregations.map(aggregation => getSqlAggregationAlias(aggregation));

  if (hasPivotsSelected) {
    aggCols.push(...aggregations.map(
        aggregation => getSqlTotalAggregationAlias(aggregation)));
  }

  if (isStackQuery) {
    aggCols.push(...aggregations.map(
        aggregation => getSqlAggregationOverStackAlias(aggregation)));
  }
  return aggCols;
}

export class PivotTableQueryGenerator {
  // Generates a query that selects all pivots and aggregations and joins any
  // tables needed by them together. All pivots are renamed into the format
  // tableName columnName and all aggregations are renamed into
  // tableName columnName (aggregation) (see getPivotAlias or
  // getAggregationAlias).
  private generateJoinQuery(
      pivots: PivotAttrs[], aggregations: AggregationAttrs[],
      whereFilters: string[], joinTables: string[]): string {

    const pivotCols = [];
    for (const pivot of pivots) {
      if (pivot.isStackPivot) {
        pivotCols.push(
            `${pivot.tableName}.name AS ` +
            `${getSqlPivotAlias(pivot)}`);

        pivotCols.push(...getHiddenStackHelperColumns(pivot).map(
            column => `${column.pivotAttrs.tableName}.${
                column.pivotAttrs.columnName} AS "${column.columnAlias}"`));
      } else {
        pivotCols.push(
            `${pivot.tableName}.${pivot.columnName} AS ` +
            `${getSqlPivotAlias(pivot)}`);
      }
    }

    const aggCols = [];
    for (const aggregation of aggregations) {
      aggCols.push(
          `${aggregation.tableName}.${aggregation.columnName} AS ` +
          `${getSqlAggregationAlias(aggregation)}`);
    }

    return `
      SELECT
        ${pivotCols.concat(aggCols).join(',\n  ')}
      FROM
        ${joinTables.join(',\n  ')}
      WHERE
        ${whereFilters.join(' AND\n  ')}
    `;
  }

  // Partitions the aggregations from the subquery generateJoinQuery over
  // all sets of appended pivots ({pivot1}, {pivot1, pivot2}, etc).
  private generateAggregationQuery(
      pivots: PivotAttrs[], aggregations: AggregationAttrs[],
      whereFilters: string[], joinTables: string[],
      isStackQuery: boolean): string {
    // No need for this query if there are no aggregations.
    if (aggregations.length === 0) {
      return this.generateJoinQuery(
          pivots, aggregations, whereFilters, joinTables);
    }

    const pivotCols = getSqlAliasedPivotColumns(pivots);
    let partitionByPivotCols = pivotCols;
    if (pivots.length > 0 && pivots[0].isStackPivot) {
      partitionByPivotCols = [];
      partitionByPivotCols.push(
          getSqlHiddenPivotAlias(getStackColumn(pivots[0])));
      partitionByPivotCols.push(...getSqlAliasedPivotColumns(pivots.slice(1)));
    }

    const aggCols = [];
    for (const aggregation of aggregations) {
      const aggColPrefix =
          `${aggregation.aggregation}(${getSqlAggregationAlias(aggregation)})`;

      if (pivots.length === 0) {
        // Don't partition over pivots if there are no pivots.
        aggCols.push(
            `${aggColPrefix} AS ${getSqlAggregationAlias(aggregation)}`);
        continue;
      }

      // Add total aggregations column.
      aggCols.push(
          `${aggColPrefix} OVER () AS ` +
          `${getSqlTotalAggregationAlias(aggregation)}`);

      // Add aggregation over stack column.
      if (isStackQuery) {
        aggCols.push(
            `${aggColPrefix} OVER (PARTITION BY ` +
            `${partitionByPivotCols[0]}) AS ` +
            `${getSqlAggregationOverStackAlias(aggregation)}`);
      }

      aggCols.push(
          `${aggColPrefix} OVER (PARTITION BY ` +
          `${partitionByPivotCols.join(',  ')}) AS ` +
          `${getSqlAggregationAlias(aggregation)}`);
    }

    return `
      SELECT
        ${pivotCols.concat(aggCols).join(',\n  ')}
      FROM (
        ${
        this.generateJoinQuery(pivots, aggregations, whereFilters, joinTables)}
      )
    `;
  }

  // Takes a list of pivots and aggregations and generates a query that
  // extracts all pivots and aggregation partitions and groups by all
  // columns and orders by each aggregation as requested.
  private generateQueryImpl(
      pivots: PivotAttrs[], aggregations: AggregationAttrs[],
      whereFilters: string[], joinTables: string[], isStackQuery: boolean,
      orderBy: boolean): string {
    // No need to generate query if there is no selected pivots or
    // aggregations.
    if (pivots.length === 0 && aggregations.length === 0) {
      return '';
    }


    const pivotCols = getSqlAliasedPivotColumns(pivots);
    const aggCols = getSqlAliasedAggregationsColumns(
        aggregations,
        /* has_pivots_selected = */ pivots.length > 0,
        isStackQuery);

    const aggregationsGroupBy =
        aggregations.map(aggregation => getSqlAggregationAlias(aggregation));

    let query = `
      SELECT
        ${pivotCols.concat(aggCols).join(',\n  ')}
      FROM (
        ${
        this.generateAggregationQuery(
            pivots, aggregations, whereFilters, joinTables, isStackQuery)}
      )
      GROUP BY
        ${pivotCols.concat(aggregationsGroupBy).join(',  ')}
    `;

    const pivotsOrderBy = [];

    // Sort by depth first if generating a stack query, to ensure that the
    // parents appear first before their children and allow us to nest the
    // results into an expandable structure.
    if (orderBy && isStackQuery) {
      pivotsOrderBy.push(
          `${getSqlHiddenPivotAlias(getStackDepthColumn(pivots[0]))} ASC`);
    }

    // For each aggregation we order by either 'DESC' or 'ASC' as
    // requested (DESC by default).
    const orderString = (aggregation: AggregationAttrs) =>
        `${getSqlAggregationAlias(aggregation)} ` +
        `${aggregation.order}`;
    const aggregationsOrderBy =
        aggregations.map(aggregation => orderString(aggregation));

    if (orderBy && pivotsOrderBy.length + aggregationsOrderBy.length > 0) {
      query += `
        ORDER BY
          ${pivotsOrderBy.concat(aggregationsOrderBy).join(',  ')}
      `;
    }
    return query;
  }

  generateQuery(
      pivots: PivotAttrs[], aggregations: AggregationAttrs[],
      whereFilters: string[], joinTables: string[]) {
    return this.generateQueryImpl(
        pivots,
        aggregations,
        whereFilters,
        joinTables,
        /* is_stack_query = */ false,
        /* order_by = */ true);
  }

  generateStackQuery(
      pivots: PivotAttrs[], aggregations: AggregationAttrs[],
      whereFilters: string[], joinTables: string[], stackId: string) {
    const stackQuery = this.generateQueryImpl(
        pivots,
        aggregations,
        whereFilters,
        joinTables,
        /* is_stack_query = */ true,
        /* order_by = */ true);

    // Query the next column rows for the parent row.
    if (pivots.length > 1) {
      const stackPivot = pivots[0];
      const currStackQuery = this.generateQueryImpl(
          pivots,
          aggregations,
          whereFilters.concat(getParentStackWhereFilter(stackPivot, stackId)),
          AVAILABLE_TABLES,
          /* is_stack_query = */ true,
          /* order_by = */ false);
      return `${currStackQuery} UNION ALL ${stackQuery}`;
    }
    return stackQuery;
  }
}