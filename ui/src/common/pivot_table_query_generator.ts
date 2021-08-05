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

export interface AggregationAttrs {
  tableName: string;
  columnName: string;
  aggregation: string;
  order: string;
}

export interface PivotAttrs {
  tableName: string;
  columnName: string;
}

function getPivotAlias(pivot: PivotAttrs): string {
  return `${pivot.tableName}_${pivot.columnName}`;
}

function getAggregationAlias(
    aggregation: AggregationAttrs, index?: number): string {
  let alias = `${aggregation.aggregation}_${aggregation.tableName}_${
      aggregation.columnName}`;
  if (index !== undefined) {
    alias += `_${index}`;
  }
  return alias;
}

function getAliasedPivotColumns(
    pivots: PivotAttrs[], lastIndex: number): string[] {
  const pivotCols = [];
  for (let i = 0; i < lastIndex; ++i) {
    pivotCols.push(getPivotAlias(pivots[i]));
  }
  return pivotCols;
}

function getAliasedAggregationColumns(
    pivots: PivotAttrs[], aggregations: AggregationAttrs[]): string[] {
  const aggCols = [];
  for (let i = 0; i < aggregations.length; ++i) {
    if (pivots.length === 0) {
      aggCols.push(getAggregationAlias(aggregations[i]));
      continue;
    }
    for (let j = 0; j < pivots.length; ++j) {
      aggCols.push(getAggregationAlias(aggregations[i], j + 1));
    }
  }
  return aggCols;
}

export class PivotTableQueryGenerator {
  // Generates a query that selects all pivots and aggregations and joins any
  // tables needed by them together. All pivots are renamed into the format
  // tableName_columnName and all aggregations are renamed into
  // aggregation_tableName_columnName (see getPivotAlias or
  // getAggregationAlias).
  private generateJoinQuery(
      pivots: PivotAttrs[], aggregations: AggregationAttrs[]): string {
    let joinQuery = 'SELECT\n';

    const pivotCols = [];
    for (let i = 0; i < pivots.length; ++i) {
      pivotCols.push(
          `${pivots[i].tableName}.${pivots[i].columnName} AS ` +
          `${getPivotAlias(pivots[i])}`);
    }

    const aggCols = [];
    for (let i = 0; i < aggregations.length; ++i) {
      aggCols.push(
          `${aggregations[i].tableName}.${aggregations[i].columnName} AS ` +
          `${getAggregationAlias(aggregations[i])}`);
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
    for (let i = 0; i < aggregations.length; ++i) {
      const aggColPrefix = `${aggregations[i].aggregation}(${
          getAggregationAlias(aggregations[i])})`;

      if (pivots.length === 0) {
        // Don't partition over pivots if there are no pivots.
        aggCols.push(
            `${aggColPrefix} AS ${getAggregationAlias(aggregations[i])}`);
        continue;
      }

      for (let j = 0; j < pivots.length; ++j) {
        aggCols.push(
            `${aggColPrefix} OVER (PARTITION BY ` +
            `${getAliasedPivotColumns(pivots, j + 1).join(',  ')}) AS ` +
            `${getAggregationAlias(aggregations[i], j + 1)}`);
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