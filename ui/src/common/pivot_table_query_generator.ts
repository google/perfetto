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

import {AggregationAttrs, PivotAttrs} from './pivot_table_data';

export function getPivotAlias(pivot: PivotAttrs): string {
  return `${pivot.tableName} ${pivot.columnName}`;
}

export function getAggregationAlias(
    aggregation: AggregationAttrs, index?: number): string {
  let alias = `${aggregation.tableName} ${aggregation.columnName} (${
      aggregation.aggregation})`;
  if (index !== undefined) {
    alias += ` ${index}`;
  }
  return alias;
}

export function getSqlPivotAlias(pivot: PivotAttrs): string {
  return `"${getPivotAlias(pivot)}"`;
}

export function getSqlAggregationAlias(
    aggregation: AggregationAttrs, index?: number): string {
  return `"${getAggregationAlias(aggregation, index)}"`;
}

function getAliasedPivotColumns(
    pivots: PivotAttrs[], lastIndex: number): string[] {
  const pivotCols = [];
  for (let i = 0; i < lastIndex; ++i) {
    pivotCols.push(getSqlPivotAlias(pivots[i]));
  }
  return pivotCols;
}

function getAliasedAggregationColumns(
    pivots: PivotAttrs[], aggregations: AggregationAttrs[]): string[] {
  const aggCols = [];
  for (const aggregation of aggregations) {
    if (pivots.length === 0) {
      aggCols.push(getSqlAggregationAlias(aggregation));
      continue;
    }
    for (let j = 0; j < pivots.length; ++j) {
      aggCols.push(getSqlAggregationAlias(aggregation, j + 1));
    }
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
      pivots: PivotAttrs[], aggregations: AggregationAttrs[]): string {
    let joinQuery = 'SELECT\n';

    const pivotCols = [];
    for (const pivot of pivots) {
      pivotCols.push(
          `${pivot.tableName}.${pivot.columnName} AS ` +
          `${getSqlPivotAlias(pivot)}`);
    }

    const aggCols = [];
    for (const aggregation of aggregations) {
      aggCols.push(
          `${aggregation.tableName}.${aggregation.columnName} AS ` +
          `${getSqlAggregationAlias(aggregation)}`);
    }

    joinQuery += pivotCols.concat(aggCols).join(',\n  ');
    joinQuery += '\n';
    joinQuery += 'FROM slice WHERE slice.dur != -1\n';
    return joinQuery;
  }

  // Partitions the aggregations from the subquery generateJoinQuery over
  // all sets of appended pivots ({pivot1}, {pivot1, pivot2}, etc).
  private generateAggregationQuery(
      pivots: PivotAttrs[], aggregations: AggregationAttrs[]): string {
    // No need for this query if there are no aggregations.
    if (aggregations.length === 0) {
      return this.generateJoinQuery(pivots, aggregations);
    }

    let aggQuery = 'SELECT\n';
    const pivotCols = getAliasedPivotColumns(pivots, pivots.length);

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

      for (let j = 0; j < pivots.length; ++j) {
        aggCols.push(
            `${aggColPrefix} OVER (PARTITION BY ` +
            `${getAliasedPivotColumns(pivots, j + 1).join(',  ')}) AS ` +
            `${getSqlAggregationAlias(aggregation, j + 1)}`);
      }
    }

    aggQuery += pivotCols.concat(aggCols).join(',\n  ');
    aggQuery += '\n';
    aggQuery += 'FROM (\n';
    aggQuery += `${this.generateJoinQuery(pivots, aggregations)}`;
    aggQuery += ')\n';
    return aggQuery;
  }

  // Takes a list of pivots and aggregations and generates a query that
  // extracts all pivots and aggregation partitions and groups by all
  // columns and orders by each aggregation as requested.
  generateQuery(pivots: PivotAttrs[], aggregations: AggregationAttrs[]):
      string {
    // No need to generate query if there is no selected pivots or
    // aggregations.
    if (pivots.length === 0 && aggregations.length === 0) {
      return '';
    }

    let query = '\nSELECT\n';

    const pivotCols = getAliasedPivotColumns(pivots, pivots.length);
    const aggCols = getAliasedAggregationColumns(pivots, aggregations);

    query += pivotCols.concat(aggCols).join(',\n  ');
    query += '\n';
    query += 'FROM (\n';
    query += `${this.generateAggregationQuery(pivots, aggregations)}`;
    query += ')\n';
    query += 'GROUP BY ';

    const aggPartitionNum = aggregations.length * Math.max(pivots.length, 1);

    // Generate an array from 1 to size (number of pivots and aggregation
    // partitions) into a string to group by all columns.
    const size = pivots.length + aggPartitionNum;
    const groupByQuery =
        new Array(size).fill(1).map((_, i) => i + 1).join(',  ');
    query += groupByQuery;
    query += '\n';

    // For each aggregation partition (found after pivot columns) we order by
    // either 'DESC' or 'ASC' as requested (DESC by default).
    if (aggregations.length > 0) {
      query += 'ORDER BY ';
      const orderString = (i: number) => `${i + 1 + pivots.length} ` +
          `${aggregations[Math.floor(i / Math.max(pivots.length, 1))].order}`;
      const orderByQuery = new Array(aggPartitionNum)
                               .fill(1)
                               .map((_, i) => orderString(i))
                               .join(',  ');
      query += orderByQuery;
      query += '\n';
    }
    return query;
  }
}