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

import {maybeUndefined} from '../../../base/utils';
import {splitPath} from './datagrid_schema';
import {quoteIdentifier} from './sql_utils';

/**
 * SQL Schema system for SQLDataSource.
 *
 * This module defines how column paths (like 'parent.name' or 'thread.process.pid')
 * map to SQL queries with appropriate JOINs. It works in parallel with the UI
 * ColumnSchema but focuses on SQL generation rather than rendering.
 *
 * A schema is a tree: joins embed the referenced table schema directly. For
 * self-referential tables (e.g. a slice's parent slice) use a `get schema()`
 * getter so the reference resolves lazily, after the object is constructed.
 *
 * Example usage:
 * ```typescript
 * const slice: SQLTableSchema = {
 *   table: 'slice',
 *   columns: {
 *     id: {},
 *     name: {},
 *     parent: {
 *       get schema() {
 *         return slice; // Lazy self-reference.
 *       },
 *       foreignKey: 'parent_id',
 *     },
 *     args: {
 *       // `key` may come from user/trace data, so it must go through
 *       // sqlValue() rather than being interpolated directly - otherwise a
 *       // key containing a quote could break out of the string literal.
 *       expression: (alias, key) =>
 *         `extract_arg(${alias}.arg_set_id, ${sqlValue(key ?? null)})`,
 *       parameterized: true,
 *     },
 *   },
 * };
 * ```
 */

/**
 * Defines how to produce SQL for a table's columns.
 */
export interface SQLTableSchema {
  /**
   * The SQL table name or subquery
   */
  readonly tableOrSubquery: string;

  /**
   * The primary key column (defaults to 'id').
   */
  readonly primaryKey?: string;

  /**
   * Column definitions.
   */
  readonly columns?: {
    [columnName: string]: SQLColumnDef | SQLJoinDef | SQLExpressionDef;
  };
}

/**
 * A simple column that exists directly in the table.
 */
export interface SQLColumnDef {
  /**
   * The actual SQL column name (defaults to the key name).
   */
  readonly column?: string;
}

/**
 * A relationship requiring a JOIN.
 */
export interface SQLJoinDef {
  /**
   * The schema of the table to join to.
   */
  readonly schema: SQLTableSchema;

  /**
   * Local column containing the foreign key (e.g., 'parent_id').
   */
  readonly foreignKey: string;

  /**
   * Use INNER JOIN instead of LEFT JOIN.
   * Default: false (LEFT JOIN).
   */
  readonly innerJoin?: boolean;
}

/**
 * A parameterized/computed column using a SQL expression.
 */
export interface SQLExpressionDef {
  /**
   * SQL expression generator.
   *
   * For simple expressions: (alias) => `${alias}.some_col`
   * For parameterized (escape `key` with sqlValue() - it may come from
   * user/trace data): (alias, key) =>
   *   `extract_arg(${alias}.arg_set_id, ${sqlValue(key ?? null)})`
   */
  readonly expression: (tableAlias: string, paramKey?: string) => string;

  /**
   * Whether this column accepts a parameter key (e.g., args.foo).
   * When true, remaining path segments after this column become the paramKey.
   */
  readonly parameterized?: boolean;

  /**
   * SQL query generator to fetch available parameter keys.
   * Only used when parameterized is true.
   *
   * @param tableOrSubquery The base table name (e.g., 'slice')
   * @param alias The base table alias (e.g., 'base')
   * @returns A SQL query that returns rows with a 'key' column
   *
   * Example for args:
   * ```
   * (baseTable) => `
   *   SELECT DISTINCT args.key
   *   FROM ${baseTable}
   *   JOIN args ON args.arg_set_id = ${baseTable}.arg_set_id
   *   WHERE args.key IS NOT NULL
   *   ORDER BY args.key
   *   LIMIT 1000
   * `
   * ```
   */
  readonly parameterKeysQuery?: (
    tableOrSubquery: string,
    alias: string,
  ) => string;
}

/**
 * Represents a JOIN that needs to be added to the query.
 */
export interface ResolvedJoin {
  /**
   * The table or subquery to join.
   */
  readonly tableOrSubquery: string;

  /**
   * Unique alias for this join instance (e.g., 't0', 't1').
   */
  readonly alias: string;

  /**
   * The alias of the table we're joining FROM.
   */
  readonly fromAlias: string;

  /**
   * The local column in fromAlias containing the foreign key.
   */
  readonly foreignKey: string;

  /**
   * The column in the joined table to match (usually 'id').
   */
  readonly primaryKey: string;

  /**
   * Whether to use INNER JOIN instead of LEFT JOIN.
   */
  readonly innerJoin: boolean;
}

/**
 * Result of resolving a column path to SQL.
 */
export interface ResolvedSQLColumn {
  /**
   * The SQL expression for this column (e.g., 'slice_1.name').
   */
  readonly sqlExpression: string;

  /**
   * JOINs required to access this column.
   */
  readonly joins: ReadonlyArray<ResolvedJoin>;
}

/**
 * Tracks JOIN deduplication during query building.
 */
interface JoinKey {
  readonly fromAlias: string;
  readonly tableOrSubquery: string;
  readonly foreignKey: string;
  readonly innerJoin: boolean;
}

function joinKeyToString(key: JoinKey): string {
  return `${key.fromAlias}|${key.tableOrSubquery}|${key.foreignKey}|${key.innerJoin}`;
}

/**
 * Builder class for resolving column paths to SQL with JOIN deduplication.
 */
export class SQLSchemaResolver {
  private readonly schema: SQLTableSchema;
  private readonly baseAlias: string;

  // Track existing joins to deduplicate
  private readonly joinMap = new Map<string, ResolvedJoin>();
  private readonly joins: ResolvedJoin[] = [];
  private aliasCounter = 0;

  constructor(schema: SQLTableSchema, baseAlias?: string) {
    this.schema = schema;

    // Aliases are neutral identifiers ('base', 't0', 't1', ...). They only need
    // to be unique and valid SQL - the table/subquery text is never embedded, so
    // subqueries can't produce invalid aliases.
    this.baseAlias = baseAlias ?? 'base';
  }

  /**
   * Gets the base table alias.
   */
  getBaseAlias(): string {
    return this.baseAlias;
  }

  /**
   * Gets the base table name.
   */
  getBaseTableOrSubquery(): string {
    return this.schema.tableOrSubquery;
  }

  /**
   * Gets all accumulated JOINs.
   */
  getJoins(): ReadonlyArray<ResolvedJoin> {
    return this.joins;
  }

  /**
   * Resolves a column path to its SQL expression, adding JOINs as needed.
   *
   * @param path The column path (e.g., 'name', 'parent.name', 'parent.parent.ts')
   * @returns The SQL expression, or undefined if the path is invalid
   */
  resolveColumnPath(path: string): string | undefined {
    const parts = splitPath(path);
    return this.resolvePath(parts, this.schema, this.baseAlias);
  }

  private resolvePath(
    parts: string[],
    schema: SQLTableSchema,
    currentAlias: string,
  ): string | undefined {
    if (parts.length === 0) {
      return undefined;
    }

    const [first, ...rest] = parts;
    const colDef = maybeUndefined(schema.columns?.[first]);

    if (!colDef) {
      // Column not found in schema - treat as raw column name.
      // This allows passthrough for columns not explicitly defined, so the
      // name (which may come from user/trace data rather than a trusted
      // schema definition) must be quoted as an identifier.
      if (rest.length === 0) {
        return `${currentAlias}.${quoteIdentifier(first)}`;
      }
      return undefined;
    }

    if ('expression' in colDef) {
      // Expression column
      if (colDef.parameterized) {
        // Remaining parts form the parameter key
        const paramKey = rest.join('.');
        return colDef.expression(currentAlias, paramKey || undefined);
      }
      // Non-parameterized expression
      if (rest.length > 0) {
        return undefined; // Can't traverse into an expression
      }
      return colDef.expression(currentAlias);
    }

    if ('schema' in colDef) {
      // JOIN column - need to add a join and continue resolving
      const targetSchema = colDef.schema;
      const joinAlias = this.getOrCreateJoin(
        currentAlias,
        targetSchema.tableOrSubquery,
        colDef.foreignKey,
        targetSchema.primaryKey ?? 'id',
        colDef.innerJoin ?? false,
      );

      if (rest.length === 0) {
        // Path ends at a join - return the primary key of the joined table
        return `${joinAlias}.${targetSchema.primaryKey ?? 'id'}`;
      }

      // Continue resolving into the joined table
      return this.resolvePath(rest, colDef.schema, joinAlias);
    }

    // Simple column
    if (rest.length > 0) {
      return undefined; // Can't traverse into a simple column
    }

    const sqlColumn = colDef.column ?? first;
    return `${currentAlias}.${sqlColumn}`;
  }

  private getOrCreateJoin(
    fromAlias: string,
    tableOrSubquery: string,
    foreignKey: string,
    primaryKey: string,
    innerJoin: boolean,
  ): string {
    const key: JoinKey = {fromAlias, tableOrSubquery, foreignKey, innerJoin};
    const keyStr = joinKeyToString(key);

    const existing = this.joinMap.get(keyStr);
    if (existing) {
      return existing.alias;
    }

    // Create new join
    const alias = `t${this.aliasCounter}`;
    this.aliasCounter++;

    const join: ResolvedJoin = {
      tableOrSubquery,
      alias,
      fromAlias,
      foreignKey,
      primaryKey,
      innerJoin,
    };

    this.joinMap.set(keyStr, join);
    this.joins.push(join);

    return alias;
  }

  /**
   * Generates the JOIN clauses for the accumulated joins.
   */
  buildJoinClauses(): string {
    return this.joins
      .map((join) => {
        const joinType = join.innerJoin ? 'JOIN' : 'LEFT JOIN';
        return `${joinType} (${join.tableOrSubquery}) AS ${join.alias} ON ${join.alias}.${join.primaryKey} = ${join.fromAlias}.${join.foreignKey}`;
      })
      .join('\n');
  }

  /**
   * Resets the resolver state, clearing all accumulated joins.
   */
  reset(): void {
    this.joinMap.clear();
    this.joins.length = 0;
    this.aliasCounter = 0;
  }
}
