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
import {assertUnreachable} from '../../../base/logging';
import {Engine} from '../../../trace_processor/engine';
import {NUM, Row, SqlValue} from '../../../trace_processor/query_result';
import {runQueryForQueryTable} from '../../query_table/queries';
import {
  DataGridDataSource,
  DataSourceResult,
  DataGridFilter,
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
  private readonly sqlImports?: string;
  private importsExecuted = false;

  // Track query components separately to avoid unnecessary re-fetches
  private lastFilterQuery = ''; // Query with just filters (for row count)
  private lastSortedQuery = ''; // Query with filters + sorting (for rows)
  private lastColumns?: ReadonlyArray<string>; // Column order (for SELECT)

  private pagination?: Pagination;
  private aggregates?: ReadonlyArray<AggregateSpec>;
  private cachedResult?: DataSourceResult;
  private isLoadingFlag = false;

  constructor(engine: Engine, query: string, sqlImports?: string) {
    this.engine = engine;
    this.baseQuery = query;
    this.sqlImports = sqlImports;
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
    distinctValuesColumns,
  }: DataGridModel): void {
    this.limiter.schedule(async () => {
      try {
        // Ensure SQL imports are executed before any queries
        if (!this.importsExecuted) {
          this.isLoadingFlag = true;
          this.sqlImports && (await this.engine.query(this.sqlImports));
          this.importsExecuted = true;
        }

        // Build query components separately to minimize re-fetches:
        // - filterQuery: affects row count and aggregates
        // - sortedQuery: affects row ordering (needs re-fetch of rows)
        // - columns: only affects SELECT clause (no re-fetch needed if data cached)
        const filterQuery = this.buildFilterQuery(filters);
        const sortedQuery = this.buildSortedQuery(filterQuery, sorting);

        // Only re-fetch row count if filters changed
        const filtersChanged = filterQuery !== this.lastFilterQuery;
        if (filtersChanged) {
          this.lastFilterQuery = filterQuery;

          // Clear the cache since filters affect everything
          this.cachedResult = undefined;
          this.pagination = undefined;
          this.aggregates = undefined;

          // Update the cache with the total row count
          this.isLoadingFlag = true;
          const rowCount = await this.getRowCount(filterQuery);
          this.cachedResult = {
            rowOffset: 0,
            totalRows: rowCount,
            rows: [],
            aggregates: {},
            distinctValues: new Map<string, ReadonlyArray<SqlValue>>(),
          };
        }

        // Check if sorting changed (but not filters)
        const sortingChanged = sortedQuery !== this.lastSortedQuery;
        if (sortingChanged) {
          this.lastSortedQuery = sortedQuery;

          // Sorting changed - need to re-fetch rows but not row count
          if (!filtersChanged) {
            // Only clear pagination cache, keep row count
            this.pagination = undefined;
          }
        }

        // Track column changes (only affects what we SELECT, not what we fetch)
        const columnsChanged = !areColumnsEqual(this.lastColumns, columns);
        if (columnsChanged) {
          this.lastColumns = columns;
          // Column order changed - need to re-fetch rows with new column order
          // but row count and aggregates stay the same
          if (!filtersChanged && !sortingChanged) {
            this.pagination = undefined;
          }
        }

        if (!areAggregateArraysEqual(this.aggregates, aggregates)) {
          this.aggregates = aggregates;
          if (aggregates) {
            this.isLoadingFlag = true;
            const aggregateResults = await this.getAggregates(
              filterQuery,
              aggregates,
            );
            this.cachedResult = {
              ...this.cachedResult!,
              aggregates: aggregateResults,
            };
          }
        }

        // Fetch data if pagination has changed or we need to re-fetch rows
        if (!comparePagination(this.pagination, pagination)) {
          this.pagination = pagination;
          this.isLoadingFlag = true;
          const {offset, rows} = await this.getRows(
            sortedQuery,
            columns,
            pagination,
          );
          this.cachedResult = {
            ...this.cachedResult!,
            rowOffset: offset,
            rows,
          };
        }

        // Handle distinct values requests
        if (distinctValuesColumns) {
          for (const column of distinctValuesColumns) {
            if (!this.cachedResult?.distinctValues?.has(column)) {
              // Schedule query to fetch distinct values
              const query = `
                SELECT DISTINCT ${column} AS value
                FROM (${this.baseQuery})
                ORDER BY ${column} IS NULL, ${column}
              `;
              this.isLoadingFlag = true;
              const result = await runQueryForQueryTable(query, this.engine);
              const values = result.rows.map((r) => r['value']);
              this.cachedResult = {
                ...this.cachedResult!,
                // Subsume the old distinct values map and add the new entry
                distinctValues: new Map<string, ReadonlyArray<SqlValue>>([
                  ...this.cachedResult!.distinctValues!,
                  [column, values],
                ]),
              };
            }
          }
        }
      } finally {
        this.isLoadingFlag = false;
      }
    });
  }

  /**
   * Returns the current query with filters and sorting applied.
   * Returns the base query if no filters/sorting have been applied yet.
   */
  getQuery(): string {
    return this.lastSortedQuery || this.baseQuery;
  }

  /**
   * Returns the SQL imports needed for this data source's queries.
   */
  getSqlImports(): string | undefined {
    return this.sqlImports;
  }

  /**
   * Export all data with current filters/sorting applied.
   */
  async exportData(): Promise<Row[]> {
    if (!this.lastSortedQuery) {
      // If no working query exists yet, we can't export anything
      return [];
    }

    const query = `SELECT * FROM (${this.lastSortedQuery})`;
    const result = await runQueryForQueryTable(query, this.engine);

    // Return all rows
    return result.rows;
  }

  /**
   * Builds a query with just filters applied (no sorting or column selection).
   * Used for row count and aggregates which don't depend on sort order.
   */
  private buildFilterQuery(filters: ReadonlyArray<DataGridFilter>): string {
    let query = `SELECT * FROM (${this.baseQuery})`;

    if (filters.length > 0) {
      const whereConditions = filters.map(filter2Sql).join(' AND ');
      query += `\nWHERE ${whereConditions}`;
    }

    return query;
  }

  /**
   * Builds a query with filters and sorting applied.
   * Used for fetching rows in the correct order.
   */
  private buildSortedQuery(filterQuery: string, sorting: Sorting): string {
    let query = filterQuery;

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
    sortedQuery: string,
    columns: ReadonlyArray<string> | undefined,
    pagination?: Pagination,
  ): Promise<{offset: number; rows: Row[]}> {
    const colDefs = columns?.join(', ') ?? '*';
    let query = `
      WITH data AS (${sortedQuery})
      SELECT ${colDefs}
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

function filter2Sql(filter: DataGridFilter): string {
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
    case 'not glob':
      return `${filter.column} NOT GLOB ${sqlValue(filter.value)}`;
    case 'is null':
      return `${filter.column} IS NULL`;
    case 'is not null':
      return `${filter.column} IS NOT NULL`;
    case 'in':
      return `${filter.column} IN (${filter.value.map(sqlValue).join(', ')})`;
    case 'not in':
      return `${filter.column} NOT IN (${filter.value.map(sqlValue).join(', ')})`;
    default:
      assertUnreachable(filter);
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

function areColumnsEqual(
  a: ReadonlyArray<string> | undefined,
  b: ReadonlyArray<string> | undefined,
): boolean {
  if (!a && !b) return true;
  if (!a || !b) return false;
  if (a.length !== b.length) return false;
  return a.every((col, i) => col === b[i]);
}
