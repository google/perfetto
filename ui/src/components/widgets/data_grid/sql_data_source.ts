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
  PivotModel,
} from './common';

export class SQLDataSource implements DataGridDataSource {
  private readonly engine: Engine;
  private readonly limiter = new AsyncLimiter();
  private readonly baseQuery: string;
  private workingQuery = '';
  private pagination?: Pagination;
  private aggregates?: ReadonlyArray<AggregateSpec>;
  private pivot?: PivotModel;
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
    pivot,
    distinctValuesColumns,
  }: DataGridModel): void {
    this.limiter.schedule(async () => {
      this.isLoadingFlag = true;

      try {
        // If the working query has changed, we need to invalidate the cache and
        // reload everything, including the page count.
        const workingQuery = this.buildWorkingQuery(
          columns,
          filters,
          sorting,
          pivot,
        );
        if (
          workingQuery !== this.workingQuery ||
          !arePivotsEqual(this.pivot, pivot)
        ) {
          this.workingQuery = workingQuery;
          this.pivot = pivot;

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
            distinctValues: new Map<string, ReadonlyArray<SqlValue>>(),
          };
        }

        if (!areAggregateArraysEqual(this.aggregates, aggregates)) {
          this.aggregates = aggregates;
          if (pivot && !pivot.drillDown) {
            // In pivot mode, compute grand totals from the base data (with filters)
            // This is more correct than aggregating already-aggregated values
            const aggregateResults = await this.getPivotAggregates(
              filters,
              pivot,
            );
            this.cachedResult = {
              ...this.cachedResult!,
              aggregates: aggregateResults,
            };
          } else if (aggregates && aggregates.length > 0) {
            // Non-pivot mode: use the working query
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
   * Export all data with current filters/sorting applied.
   */
  async exportData(): Promise<Row[]> {
    if (!this.workingQuery) {
      // If no working query exists yet, we can't export anything
      return [];
    }

    const query = `SELECT * FROM (${this.workingQuery})`;
    const result = await runQueryForQueryTable(query, this.engine);

    // Return all rows
    return result.rows;
  }

  /**
   * Builds a complete SQL query that defines the working dataset (ignores
   * pagination).
   */
  private buildWorkingQuery(
    columns: ReadonlyArray<string> | undefined,
    filters: ReadonlyArray<DataGridFilter>,
    sorting: Sorting,
    pivot?: PivotModel,
  ): string {
    const colDefs = columns ?? ['*'];

    let query: string;

    if (pivot && !pivot.drillDown) {
      // Pivot mode (no drill-down): Build aggregate columns
      const valCols = Object.entries(pivot.values)
        .map(([alias, value]) => {
          if (value.func === 'COUNT') {
            return `COUNT(*) AS ${alias}`;
          }
          if (value.func === 'ANY') {
            return `MIN(${value.col}) AS ${alias}`;
          }
          return `${value.func}(${value.col}) AS ${alias}`;
        })
        .join(', ');

      if (pivot.groupBy.length > 0) {
        // Grouped aggregation
        const groupCols = pivot.groupBy.join(', ');
        const selectCols = valCols ? `${groupCols}, ${valCols}` : groupCols;
        query = `
          SELECT ${selectCols}
          FROM (${this.baseQuery})
          GROUP BY ${groupCols}
        `;
      } else {
        // Aggregation without grouping (single row result)
        query = `
          SELECT ${valCols}
          FROM (${this.baseQuery})
        `;
      }
    } else if (pivot?.drillDown) {
      // Drill-down mode: Show raw rows filtered by the groupBy values
      query = `\nSELECT ${colDefs.join(', ')} FROM (${this.baseQuery})`;

      // Build WHERE clause from drillDown values
      const drillDownConditions = pivot.groupBy
        .map((col) => {
          const value = pivot.drillDown![col];
          if (value === null) {
            return `${col} IS NULL`;
          }
          return `${col} = ${sqlValue(value)}`;
        })
        .join(' AND ');

      if (drillDownConditions) {
        query = `SELECT * FROM (${query}) WHERE ${drillDownConditions}`;
      }
    } else {
      query = `\nSELECT ${colDefs.join(', ')} FROM (${this.baseQuery})`;
    }

    // Add WHERE clause if there are filters
    if (filters.length > 0) {
      const whereConditions = filters.map(filter2Sql).join(' AND ');
      query = `SELECT * FROM (${query}) WHERE ${whereConditions}`;
    }

    // Add ORDER BY clause for sorting, but only if the sort column exists
    if (sorting.direction !== 'UNSORTED') {
      const {column, direction} = sorting as SortByColumn;
      // Check if the sort column is in the available columns
      // If columns is undefined, we're using '*', so assume all columns are available
      const columnExists = columns === undefined || columns.includes(column);
      if (columnExists) {
        query += `\nORDER BY ${column} ${direction.toUpperCase()}`;
      }
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
    const selectClauses = aggregates.map((a) => {
      return `${a.func}(${a.col}) AS ${a.col}`;
    });

    const query = `
      WITH data AS (${workingQuery})
      SELECT
        ${selectClauses.join(', ')}
      FROM data
    `;

    const result = await runQueryForQueryTable(query, this.engine);
    return result.rows[0] ?? {};
  }

  /**
   * Compute grand total aggregates for pivot mode by querying the base data
   * directly (with filters applied). This is more accurate than aggregating
   * already-aggregated values (e.g., AVG of AVGs is not the same as grand AVG).
   */
  private async getPivotAggregates(
    filters: ReadonlyArray<DataGridFilter>,
    pivot: PivotModel,
  ): Promise<Row> {
    // Build a filtered base query (no GROUP BY, no sorting)
    let filteredBaseQuery = `SELECT * FROM (${this.baseQuery})`;
    if (filters.length > 0) {
      const whereConditions = filters.map(filter2Sql).join(' AND ');
      filteredBaseQuery = `SELECT * FROM (${filteredBaseQuery}) WHERE ${whereConditions}`;
    }

    // Build aggregate expressions from pivot.values using original column names
    const selectClauses = Object.entries(pivot.values)
      .map(([alias, value]) => {
        if (value.func === 'COUNT') {
          return `COUNT(*) AS ${alias}`;
        }
        if (value.func === 'ANY') {
          // ANY doesn't make sense as a grand total, just return NULL
          return `NULL AS ${alias}`;
        }
        return `${value.func}(${value.col}) AS ${alias}`;
      })
      .join(', ');

    if (!selectClauses) {
      return {};
    }

    const query = `
      SELECT ${selectClauses}
      FROM (${filteredBaseQuery})
    `;

    const result = await runQueryForQueryTable(query, this.engine);
    return result.rows[0] ?? {};
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

function arePivotsEqual(a?: PivotModel, b?: PivotModel): boolean {
  if (a === b) return true;
  if (a === undefined || b === undefined) return false;
  if (a.groupBy.join(',') !== b.groupBy.join(',')) return false;
  if (JSON.stringify(a.values) !== JSON.stringify(b.values)) return false;
  if (JSON.stringify(a.drillDown) !== JSON.stringify(b.drillDown)) return false;
  return true;
}
