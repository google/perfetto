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

/**
 * SQL Schema system for SQLDataSource.
 *
 * This module defines how column paths (like 'parent.name' or 'thread.process.pid')
 * map to SQL queries with appropriate JOINs. It works in parallel with the UI
 * SchemaRegistry but focuses on SQL generation rather than rendering.
 *
 * Example usage:
 * ```typescript
 * const schema: SQLSchemaRegistry = {
 *   slice: {
 *     table: 'slice',
 *     columns: {
 *       id: {},
 *       name: {},
 *       parent: { ref: 'slice', foreignKey: 'parent_id' },
 *       args: {
 *         expression: (alias, key) => `extract_arg(${alias}.arg_set_id, '${key}')`,
 *         parameterized: true,
 *       },
 *     },
 *   },
 * };
 * ```
 */

/**
 * Registry of named SQL schemas that can reference each other.
 * Parallel to SchemaRegistry but for SQL generation.
 */
export interface SQLSchemaRegistry {
  [schemaName: string]: SQLTableSchema;
}

/**
 * Defines how to produce SQL for a table's columns.
 */
export interface SQLTableSchema {
  /**
   * The SQL table name.
   */
  readonly table: string;

  /**
   * The primary key column (defaults to 'id').
   */
  readonly primaryKey?: string;

  /**
   * Column definitions.
   */
  readonly columns: {
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
   * Name of the schema in the registry to join to.
   */
  readonly ref: string;

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
   * For parameterized: (alias, key) => `extract_arg(${alias}.arg_set_id, '${key}')`
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
   * @param baseTable The base table name (e.g., 'slice')
   * @param baseAlias The base table alias (e.g., 'slice_0')
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
    baseTable: string,
    baseAlias: string,
  ) => string;
}

/**
 * Type guard for SQLJoinDef.
 */
export function isSQLJoinDef(
  def: SQLColumnDef | SQLJoinDef | SQLExpressionDef,
): def is SQLJoinDef {
  return 'ref' in def && 'foreignKey' in def;
}

/**
 * Type guard for SQLExpressionDef.
 */
export function isSQLExpressionDef(
  def: SQLColumnDef | SQLJoinDef | SQLExpressionDef,
): def is SQLExpressionDef {
  return 'expression' in def;
}

/**
 * Type guard for SQLColumnDef.
 */
export function isSQLColumnDef(
  def: SQLColumnDef | SQLJoinDef | SQLExpressionDef,
): def is SQLColumnDef {
  return !isSQLJoinDef(def) && !isSQLExpressionDef(def);
}

/**
 * Represents a JOIN that needs to be added to the query.
 */
export interface ResolvedJoin {
  /**
   * The table to join.
   */
  readonly table: string;

  /**
   * Unique alias for this join instance (e.g., 'slice_1', 'slice_2').
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
  readonly table: string;
  readonly foreignKey: string;
  readonly innerJoin: boolean;
}

function joinKeyToString(key: JoinKey): string {
  return `${key.fromAlias}|${key.table}|${key.foreignKey}|${key.innerJoin}`;
}

/**
 * Builder class for resolving column paths to SQL with JOIN deduplication.
 */
export class SQLSchemaResolver {
  private readonly registry: SQLSchemaRegistry;
  private readonly rootSchemaName: string;
  private readonly baseAlias: string;

  // Track existing joins to deduplicate
  private readonly joinMap = new Map<string, ResolvedJoin>();
  private readonly joins: ResolvedJoin[] = [];
  private aliasCounter = 0;

  constructor(
    registry: SQLSchemaRegistry,
    rootSchemaName: string,
    baseAlias?: string,
  ) {
    this.registry = registry;
    this.rootSchemaName = rootSchemaName;
    this.baseAlias =
      baseAlias ?? `${registry[rootSchemaName]?.table ?? 'base'}_0`;
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
  getBaseTable(): string {
    const schema = this.registry[this.rootSchemaName];
    return schema?.table ?? this.rootSchemaName;
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
    const parts = path.split('.');
    return this.resolvePath(parts, this.rootSchemaName, this.baseAlias);
  }

  private resolvePath(
    parts: string[],
    schemaName: string,
    currentAlias: string,
  ): string | undefined {
    if (parts.length === 0) {
      return undefined;
    }

    const schema = maybeUndefined(this.registry[schemaName]);
    if (!schema) {
      return undefined;
    }

    const [first, ...rest] = parts;
    const colDef = maybeUndefined(schema.columns[first]);

    if (!colDef) {
      // Column not found in schema - treat as raw column name
      // This allows passthrough for columns not explicitly defined
      if (rest.length === 0) {
        return `${currentAlias}.${first}`;
      }
      return undefined;
    }

    if (isSQLExpressionDef(colDef)) {
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

    if (isSQLJoinDef(colDef)) {
      // JOIN column - need to add a join and continue resolving
      const targetSchema = maybeUndefined(this.registry[colDef.ref]);
      if (!targetSchema) {
        return undefined;
      }

      const joinAlias = this.getOrCreateJoin(
        currentAlias,
        targetSchema.table,
        colDef.foreignKey,
        targetSchema.primaryKey ?? 'id',
        colDef.innerJoin ?? false,
      );

      if (rest.length === 0) {
        // Path ends at a join - return the primary key of the joined table
        return `${joinAlias}.${targetSchema.primaryKey ?? 'id'}`;
      }

      // Continue resolving into the joined table
      return this.resolvePath(rest, colDef.ref, joinAlias);
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
    table: string,
    foreignKey: string,
    primaryKey: string,
    innerJoin: boolean,
  ): string {
    const key: JoinKey = {fromAlias, table, foreignKey, innerJoin};
    const keyStr = joinKeyToString(key);

    const existing = this.joinMap.get(keyStr);
    if (existing) {
      return existing.alias;
    }

    // Create new join
    this.aliasCounter++;
    const alias = `${table}_${this.aliasCounter}`;

    const join: ResolvedJoin = {
      table,
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
        return `${joinType} ${join.table} AS ${join.alias} ON ${join.alias}.${join.primaryKey} = ${join.fromAlias}.${join.foreignKey}`;
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

/**
 * Creates a simple schema from a table name or subquery.
 *
 * This enables using SQLDataSource with arbitrary queries/tables without
 * defining explicit column schemas. Columns are accessed directly by name.
 *
 * @param tableOrQuery A table name (e.g., 'slice') or subquery (e.g., 'SELECT * FROM slice')
 * @param schemaName Optional name for the schema (defaults to 'query')
 * @returns A SQLSchemaRegistry with a single schema entry
 *
 * Example usage:
 * ```typescript
 * const schema = createSimpleSchema('SELECT * FROM slice WHERE dur > 0');
 * const dataSource = new SQLDataSource({
 *   engine,
 *   sqlSchema: schema,
 *   rootSchemaName: 'query',
 * });
 * ```
 */
export function createSimpleSchema(
  tableOrQuery: string,
  schemaName: string = 'query',
): SQLSchemaRegistry {
  // If it looks like a query (contains SELECT, spaces, etc.), wrap in parens
  const isQuery =
    tableOrQuery.trim().toUpperCase().startsWith('SELECT') ||
    tableOrQuery.includes(' ');
  const table = isQuery ? `(${tableOrQuery})` : tableOrQuery;

  return {
    [schemaName]: {
      table,
      columns: {}, // Empty columns - all column access falls through to direct access
    },
  };
}
