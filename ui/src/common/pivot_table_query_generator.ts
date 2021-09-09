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

export function getAggregationAlias(aggregation: AggregationAttrs): string {
  return `${aggregation.tableName} ${aggregation.columnName} (${
      aggregation.aggregation})`;
}

export function getSqlPivotAlias(pivot: PivotAttrs): string {
  return `"${getPivotAlias(pivot)}"`;
}

export function getSqlAggregationAlias(aggregation: AggregationAttrs): string {
  return `"${getAggregationAlias(aggregation)}"`;
}

export class PivotTableQueryGenerator {
  // Generates a query that selects all pivots and aggregations and joins any
  // tables needed by them together. All pivots are renamed into the format
  // tableName columnName and all aggregations are renamed into
  // tableName columnName (aggregation) (see getPivotAlias or
  // getAggregationAlias).
  private generateJoinQuery(
      pivots: PivotAttrs[], aggregations: AggregationAttrs[],
      whereFilters: string[]): string {
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
    joinQuery += 'FROM slice\n';
    joinQuery += 'WHERE\n';
    joinQuery += whereFilters.join(' AND\n  ');
    joinQuery += '\n';
    return joinQuery;
  }

  // Partitions the aggregations from the subquery generateJoinQuery over
  // all sets of appended pivots ({pivot1}, {pivot1, pivot2}, etc).
  private generateAggregationQuery(
      pivots: PivotAttrs[], aggregations: AggregationAttrs[],
      whereFilters: string[]): string {
    // No need for this query if there are no aggregations.
    if (aggregations.length === 0) {
      return this.generateJoinQuery(pivots, aggregations, whereFilters);
    }

    let aggQuery = 'SELECT\n';
    const pivotCols = pivots.map(pivot => getSqlPivotAlias(pivot));

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

      aggCols.push(
          `${aggColPrefix} OVER (PARTITION BY ` +
          `${pivotCols.join(',  ')}) AS ` +
          `${getSqlAggregationAlias(aggregation)}`);
    }

    aggQuery += pivotCols.concat(aggCols).join(',\n  ');
    aggQuery += '\n';
    aggQuery += 'FROM (\n';
    aggQuery += this.generateJoinQuery(pivots, aggregations, whereFilters);
    aggQuery += ')\n';
    return aggQuery;
  }

  // Takes a list of pivots and aggregations and generates a query that
  // extracts all pivots and aggregation partitions and groups by all
  // columns and orders by each aggregation as requested.
  generateQuery(
      pivots: PivotAttrs[], aggregations: AggregationAttrs[],
      whereFilters: string[]): string {
    // No need to generate query if there is no selected pivots or
    // aggregations.
    if (pivots.length === 0 && aggregations.length === 0) {
      return '';
    }

    let query = '\nSELECT\n';

    const pivotCols = pivots.map(pivot => getSqlPivotAlias(pivot));
    const aggCols =
        aggregations.map(aggregation => getSqlAggregationAlias(aggregation));

    query += pivotCols.concat(aggCols).join(',\n  ');
    query += '\n';
    query += 'FROM (\n';
    query += this.generateAggregationQuery(pivots, aggregations, whereFilters);
    query += ')\n';
    query += 'GROUP BY ';

    // Generate an array from 1 to size (number of pivots and aggregation
    // partitions) into a string to group by all columns.
    const size = pivots.length + aggregations.length;
    const groupByQuery =
        new Array(size).fill(1).map((_, i) => i + 1).join(',  ');
    query += groupByQuery;
    query += '\n';

    // For each aggregation partition (found after pivot columns) we order by
    // either 'DESC' or 'ASC' as requested (DESC by default).
    if (aggregations.length > 0) {
      query += 'ORDER BY ';
      const orderString = (i: number) => `${i + 1 + pivots.length} ` +
          `${aggregations[i].order}`;
      const orderByQuery = new Array(aggregations.length)
                               .fill(1)
                               .map((_, i) => orderString(i))
                               .join(',  ');
      query += orderByQuery;
      query += '\n';
    }
    return query;
  }
}