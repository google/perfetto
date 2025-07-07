// Copyright (C) 2025 The Android Open Source Project
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

import {AsyncLimiter} from '../../../base/async_limiter';
import {Engine} from '../../../trace_processor/engine';
import {NUM, Row, SqlValue} from '../../../trace_processor/query_result';
import {runQueryForQueryTable} from '../../query_table/queries';
import {
  DataGridDataSource,
  DataSourceResult,
  FilterDefinition,
  Sorting,
  SortByColumn,
  DataGridModel,
  Pagination,
  AggregateSpec,
  areAggregateArraysEqual,
} from './common';

export class SQLDataSource implements DataGridDataSource {
  private readonly engine: Engine;
  private readonly limiter = new AsyncLimiter();
  private readonly baseQuery: string;
  private workingQuery = '';
  private pagination?: Pagination;
  private aggregates?: ReadonlyArray<AggregateSpec>;
  private cachedResult?: DataSourceResult;
  private isLoadingFlag = false;

  constructor(engine: Engine, query: string) {
    this.engine = engine;
    this.baseQuery = query;
  }

  /**
   * Getter for the current rows result
   */
  get rows(): DataSourceResult | undefined {
    return this.cachedResult;
  }

  get isLoading(): boolean {
    return this.isLoadingFlag;
  }

  /**
   * Notify of parameter changes and trigger data update
   */
  notifyUpdate({
    columns,
    sorting = {direction: 'UNSORTED'},
    filters = [],
    pagination,
    aggregates,
  }: DataGridModel): void {
    this.limiter.schedule(async () => {
      this.isLoadingFlag = true;

      try {
        // If the working query has changed, we need to invalidate the cache and
        // reload everything, including the page count.
        const workingQuery = this.buildWorkingQuery(columns, filters, sorting);
        if (workingQuery !== this.workingQuery) {
          this.workingQuery = workingQuery;

          // Clear the cache
          this.cachedResult = undefined;
          this.pagination = undefined;
          this.aggregates = undefined;

          // Update the cache with the total row count
          const rowCount = await this.getRowCount(workingQuery);
          this.cachedResult = {
            rowOffset: 0,
            totalRows: rowCount,
            rows: [],
            aggregates: {},
          };
        }

        if (!areAggregateArraysEqual(this.aggregates, aggregates)) {
          this.aggregates = aggregates;
          if (aggregates) {
            const aggregateResults = await this.getAggregates(
              workingQuery,
              aggregates,
            );
            this.cachedResult = {
              ...this.cachedResult!,
              aggregates: aggregateResults,
            };
          }
        }

        // Fetch data if pagination has changed.
        if (!comparePagination(this.pagination, pagination)) {
          this.pagination = pagination;
          const {offset, rows} = await this.getRows(workingQuery, pagination);
          this.cachedResult = {
            ...this.cachedResult!,
            rowOffset: offset,
            rows,
          };
        }
      } finally {
        this.isLoadingFlag = false;
      }
    });
  }

  /**
   * Builds a complete SQL query that defines the working dataset (ignores
   * pagination).
   */
  private buildWorkingQuery(
    columns: ReadonlyArray<string> | undefined,
    filters: ReadonlyArray<FilterDefinition>,
    sorting: Sorting,
  ): string {
    const colDefs = columns ?? ['*'];

    let query = `\nSELECT ${colDefs.join()} FROM (${this.baseQuery})`;

    // Add WHERE clause if there are filters
    if (filters.length > 0) {
      const whereConditions = filters.map(filter2Sql).join(' AND ');

      query += `\nWHERE ${whereConditions}`;
    }

    // Add ORDER BY clause for sorting
    if (sorting.direction !== 'UNSORTED') {
      const {column, direction} = sorting as SortByColumn;
      query += `\nORDER BY ${column} ${direction.toUpperCase()}`;
    }

    return query;
  }

  private async getRowCount(workingQuery: string): Promise<number> {
    const result = await this.engine.query(`
      WITH data AS (${workingQuery})
      SELECT COUNT(*) AS total_count
      FROM data
    `);
    return result.firstRow({total_count: NUM}).total_count;
  }

  private async getAggregates(
    workingQuery: string,
    aggregates: ReadonlyArray<AggregateSpec>,
  ): Promise<Row> {
    const query = `
      WITH data AS (${workingQuery})
      SELECT
        ${aggregates.map((a) => `${a.func}(${a.col}) AS ${a.col}`)}
      FROM data
    `;

    const result = await runQueryForQueryTable(query, this.engine);
    return result.rows[0];
  }

  private async getRows(
    workingQuery: string,
    pagination?: Pagination,
  ): Promise<{offset: number; rows: Row[]}> {
    let query = `
      WITH data AS (${workingQuery})
      SELECT *
      FROM data
    `;

    if (pagination) {
      query += `LIMIT ${pagination.limit} OFFSET ${pagination.offset}`;
    }

    const result = await runQueryForQueryTable(query, this.engine);

    return {
      offset: pagination?.offset ?? 0,
      rows: result.rows,
    };
  }
}

function filter2Sql(filter: FilterDefinition): string {
  switch (filter.op) {
    case '=':
    case '!=':
    case '<':
    case '<=':
    case '>':
    case '>=':
      return `${filter.column} ${filter.op} ${sqlValue(filter.value)}`;
    case 'glob':
      return `${filter.column} GLOB ${sqlValue(filter.value)}`;
    case 'is null':
      return `${filter.column} IS NULL`;
    case 'is not null':
      return `${filter.column} IS NOT NULL`;
    default:
      return '1=1'; // Default to true if unknown operator
  }
}

function sqlValue(value: SqlValue): string {
  if (typeof value === 'string') {
    // Escape single quotes in strings
    return `'${value.replace(/'/g, "''")}'`;
  } else if (typeof value === 'number' || typeof value === 'bigint') {
    return value.toString();
  } else if (typeof value === 'boolean') {
    return value ? '1' : '0';
  } else {
    // For other types, convert to string
    return `'${String(value)}'`;
  }
}

function comparePagination(a?: Pagination, b?: Pagination): boolean {
  // Both undefined - they're equal
  if (!a && !b) return true;

  // One is undefined, other isn't - they're different
  if (!a || !b) return false;

  // Both exist - compare their properties
  return a.limit === b.limit && a.offset === b.offset;
}
