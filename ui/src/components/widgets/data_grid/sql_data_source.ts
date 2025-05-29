import {AsyncLimiter} from '../../../base/async_limiter';
import {Engine} from '../../../trace_processor/engine';
import {NUM, SqlValue} from '../../../trace_processor/query_result';
import {runQueryForQueryTable} from '../../query_table/queries';
import {
  DataGridDataSource,
  DataSourceResult,
  FilterDefinition,
  SortBy,
  SortByColumn,
} from './common';

export class SQLDataSource implements DataGridDataSource {
  private readonly engine: Engine;
  private readonly baseQuery: string;
  private readonly limiter = new AsyncLimiter();

  // Previous query (for diffing)
  private oldQuery = '';

  // Query state
  private cachedResult: DataSourceResult = {
    totalRows: 0,
    rows: [],
    rowOffset: 0,
  };

  constructor(engine: Engine, query: string) {
    this.engine = engine;
    this.baseQuery = query;
  }

  /**
   * Getter for the current rows result
   */
  get rows(): DataSourceResult {
    return this.cachedResult;
  }

  /**
   * Notify of parameter changes and trigger data update
   */
  notifyUpdate(
    sortBy: SortBy,
    filters: ReadonlyArray<FilterDefinition>,
    offset: number,
    limit: number,
  ): void {
    const query = this.buildQuery(filters, sortBy, limit, offset);
    if (query !== this.oldQuery) {
      this.oldQuery = query;
      this.limiter.schedule(async () => {
        try {
          const result = await this.executeQueries(
            filters,
            sortBy,
            limit,
            offset,
          );

          if (result) {
            this.cachedResult = result;
          }
        } catch (error) {
          console.error('Error executing query:', error);
        }
      });
    }
  }

  /**
   * Builds a complete SQL query with filtering, sorting, and pagination
   */
  private buildQuery(
    filters: ReadonlyArray<FilterDefinition>,
    sortBy: SortBy,
    limit: number,
    offset: number,
  ): string {
    // Wrap the base query as a subquery
    let query = `WITH base_data AS (${this.baseQuery})`;

    // Start the main query
    query += `\nSELECT * FROM base_data`;

    // Add WHERE clause if there are filters
    if (filters.length > 0) {
      const whereConditions = filters
        .map((filter) => {
          switch (filter.op) {
            case '=':
              return `${filter.column} = ${this.sqlValue(filter.value)}`;
            case '!=':
              return `${filter.column} != ${this.sqlValue(filter.value)}`;
            case '<':
              return `${filter.column} < ${this.sqlValue(filter.value)}`;
            case '<=':
              return `${filter.column} <= ${this.sqlValue(filter.value)}`;
            case '>':
              return `${filter.column} > ${this.sqlValue(filter.value)}`;
            case '>=':
              return `${filter.column} >= ${this.sqlValue(filter.value)}`;
            case 'glob':
              return `${filter.column} GLOB ${this.sqlValue(filter.value)}`;
            case 'is null':
              return `${filter.column} IS NULL`;
            case 'is not null':
              return `${filter.column} IS NOT NULL`;
            default:
              return '1=1'; // Default to true if unknown operator
          }
        })
        .join(' AND ');

      query += `\nWHERE ${whereConditions}`;
    }

    // Add ORDER BY clause for sorting
    if (sortBy.direction !== 'unsorted') {
      const {column, direction} = sortBy as SortByColumn;
      query += `\nORDER BY ${column} ${direction.toUpperCase()}`;
    }

    // Add pagination with LIMIT and OFFSET
    query += `\nLIMIT ${limit} OFFSET ${offset}`;

    return query;
  }

  /**
   * Builds a count query to get the total number of rows (for pagination)
   */
  private buildCountQuery(filters: ReadonlyArray<FilterDefinition>): string {
    // Wrap the base query as a subquery
    let query = `WITH base_data AS (${this.baseQuery})`;

    // Start the count query
    query += `\nSELECT COUNT(*) as total_count FROM base_data`;

    // Add WHERE clause if there are filters
    if (filters.length > 0) {
      const whereConditions = filters
        .map((filter) => {
          switch (filter.op) {
            case '=':
            case '!=':
            case '<':
            case '<=':
            case '>':
            case '>=':
              return `${filter.column} ${filter.op} ${this.sqlValue(filter.value)}`;
            case 'glob':
              return `${filter.column} GLOB ${this.sqlValue(filter.value)}`;
            case 'is null':
              return `${filter.column} IS NULL`;
            case 'is not null':
              return `${filter.column} IS NOT NULL`;
            default:
              return '1=1'; // Default to true if unknown operator
          }
        })
        .join(' AND ');

      query += `\nWHERE ${whereConditions}`;
    }

    return query;
  }

  /**
   * Converts a JavaScript value to a SQL string representation
   */
  private sqlValue(value: SqlValue): string {
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

  private async executeQueries(
    filters: ReadonlyArray<FilterDefinition>,
    sortBy: SortBy,
    limit: number,
    offset: number,
  ): Promise<DataSourceResult | undefined> {
    const countQuery = this.buildCountQuery(filters);
    const countResult = await this.engine.query(countQuery);
    const firstRow = countResult.maybeFirstRow({total_count: NUM});
    if (!firstRow) {
      return undefined;
    }

    const totalRows = firstRow.total_count;

    // Build the data query
    const dataQuery = this.buildQuery(filters, sortBy, limit, offset);
    const dataResult = await runQueryForQueryTable(dataQuery, this.engine);

    if (dataResult.error) {
      console.error('Error executing data query:', dataResult.error);
      return undefined;
    }

    return {
      totalRows,
      rows: dataResult.rows,
      rowOffset: offset,
    };
  }
}
