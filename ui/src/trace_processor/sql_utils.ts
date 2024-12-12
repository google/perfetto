// Copyright (C) 2023 The Android Open Source Project
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

import {SortDirection} from '../base/comparison_utils';
import {isString} from '../base/object_utils';
import {sqliteString} from '../base/string_utils';
import {Engine} from './engine';
import {NUM, SqlValue} from './query_result';

export interface OrderClause {
  fieldName: string;
  direction?: SortDirection;
}

export type CommonTableExpressions = {
  [key: string]: string | undefined;
};

// Interface for defining constraints which can be passed to a SQL query.
export interface SQLConstraints {
  commonTableExpressions?: CommonTableExpressions;
  filters?: (undefined | string)[];
  joins?: (undefined | string)[];
  orderBy?: (undefined | string | OrderClause)[];
  groupBy?: (undefined | string)[];
  limit?: number;
}

function isDefined<T>(t: T | undefined): t is T {
  return t !== undefined;
}

export function constraintsToQueryPrefix(c: SQLConstraints): string {
  const ctes = Object.entries(c.commonTableExpressions ?? {}).filter(
    ([_, value]) => isDefined(value),
  );
  if (ctes.length === 0) return '';
  const cteStatements = ctes.map(([name, query]) => `${name} AS (${query})`);
  return `WITH ${cteStatements.join(',\n')}`;
}

// Formatting given constraints into a string which can be injected into
// SQL query.
export function constraintsToQuerySuffix(c: SQLConstraints): string {
  const result: string[] = [];

  const joins = (c.joins ?? []).filter(isDefined);
  if (joins.length > 0) {
    result.push(...joins);
  }
  const filters = (c.filters ?? []).filter(isDefined);
  if (filters.length > 0) {
    result.push(`WHERE ${filters.join(' and ')}`);
  }
  const groupBy = (c.groupBy ?? []).filter(isDefined);
  if (groupBy.length > 0) {
    const groups = groupBy.join(', ');
    result.push(`GROUP BY ${groups}`);
  }
  const orderBy = (c.orderBy ?? []).filter(isDefined);
  if (orderBy.length > 0) {
    const orderBys = orderBy.map((clause) => {
      if (isString(clause)) {
        return clause;
      } else {
        const direction = clause.direction ? ` ${clause.direction}` : '';
        return `${clause.fieldName}${direction}`;
      }
    });
    result.push(`ORDER BY ${orderBys.join(', ')}`);
  }
  // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
  if (c.limit) {
    result.push(`LIMIT ${c.limit}`);
  }
  return result.join('\n');
}

// Trace Processor returns number | null for NUM_NULL, while most of the UI
// code uses number | undefined. This functions provides a short-hand
// conversion.
// TODO(altimin): Support NUM_UNDEFINED as a first-class citizen.
export function fromNumNull(n: number | null): number | undefined {
  if (n === null) {
    return undefined;
  }
  return n;
}

// Given a SqlValue, return a string representation of it to display to the
// user.
export function sqlValueToReadableString(val: SqlValue): string;
export function sqlValueToReadableString(val?: SqlValue): string | undefined;
export function sqlValueToReadableString(val?: SqlValue): string | undefined {
  if (val === undefined) return undefined;
  if (val instanceof Uint8Array) {
    return `<blob length=${val.length}>`;
  }
  if (val === null) {
    return 'NULL';
  }
  return val.toString();
}

// Given a SqlValue, return a string representation (properly escaped, if
// necessary) of it to be used in a SQL query.
export function sqlValueToSqliteString(val: SqlValue): string {
  if (val instanceof Uint8Array) {
    throw new Error("Can't pass blob back to trace processor as value");
  }
  if (val === null) {
    return 'NULL';
  }
  if (typeof val === 'string') {
    return sqliteString(val);
  }
  return `${val}`;
}

// Return a SQL predicate that can be used to compare with the given `value`,
// correctly handling NULLs.
export function matchesSqlValue(value: SqlValue): string {
  if (value === null) {
    return 'IS NULL';
  }
  return `= ${sqlValueToSqliteString(value)}`;
}

export async function getTableRowCount(
  engine: Engine,
  tableName: string,
): Promise<number | undefined> {
  const result = await engine.query(
    `SELECT COUNT() as count FROM ${tableName}`,
  );
  if (result.numRows() === 0) {
    return undefined;
  }
  return result.firstRow({
    count: NUM,
  }).count;
}

export {SqlValue};

/**
 * Asynchronously creates a 'perfetto' table using the given engine and returns
 * an disposable object to handle its cleanup.
 *
 * @param engine - The database engine to execute the query.
 * @param tableName - The name of the table to be created.
 * @param expression - The SQL expression to define the table.
 * @returns An AsyncDisposable which drops the created table when disposed.
 *
 * @example
 * const engine = new Engine();
 * const tableName = 'my_perfetto_table';
 * const expression = 'SELECT * FROM source_table';
 *
 * const table = await createPerfettoTable(engine, tableName, expression);
 *
 * // Use the table...
 *
 * // Cleanup the table when done
 * await table[Symbol.asyncDispose]();
 */
export async function createPerfettoTable(
  engine: Engine,
  tableName: string,
  expression: string,
): Promise<AsyncDisposable> {
  await engine.query(`CREATE PERFETTO TABLE ${tableName} AS ${expression}`);
  return {
    [Symbol.asyncDispose]: async () => {
      await engine.tryQuery(`DROP TABLE IF EXISTS ${tableName}`);
    },
  };
}

/**
 * Asynchronously creates a SQL view using the given engine and returns an
 * disposable object to handle its cleanup.
 *
 * @param engine - The database engine to execute the query.
 * @param viewName - The name of the view to be created.
 * @param as - The SQL expression to define the table.
 * @returns An AsyncDisposable which drops the created table when disposed.
 *
 * @example
 * const engine = new Engine();
 * const viewName = 'my_view';
 * const expression = 'SELECT * FROM source_table';
 *
 * const view = await createView(engine, viewName, expression);
 *
 * // Use the view...
 *
 * // Cleanup the view when done
 * await view[Symbol.asyncDispose]();
 */
export async function createView(
  engine: Engine,
  viewName: string,
  as: string,
): Promise<AsyncDisposable> {
  await engine.query(`CREATE VIEW ${viewName} AS ${as}`);
  return {
    [Symbol.asyncDispose]: async () => {
      await engine.tryQuery(`DROP VIEW IF EXISTS ${viewName}`);
    },
  };
}

export async function createVirtualTable(
  engine: Engine,
  tableName: string,
  using: string,
): Promise<AsyncDisposable> {
  await engine.query(`CREATE VIRTUAL TABLE ${tableName} USING ${using}`);
  return {
    [Symbol.asyncDispose]: async () => {
      await engine.tryQuery(`DROP TABLE IF EXISTS ${tableName}`);
    },
  };
}

/**
 * Asynchronously creates a 'perfetto' index using the given engine and returns
 * an disposable object to handle its cleanup.
 *
 * @param engine - The database engine to execute the query.
 * @param indexName - The name of the index to be created.
 * @param expression - The SQL expression containing the table and columns.
 * @returns An AsyncDisposable which drops the created table when disposed.
 *
 * @example
 * const engine = new Engine();
 * const indexName = 'my_perfetto_index';
 * const expression = 'my_perfetto_table(foo)';
 *
 * const index = await createPerfettoIndex(engine, indexName, expression);
 *
 * // Use the index...
 *
 * // Cleanup the index when done
 * await index[Symbol.asyncDispose]();
 */
export async function createPerfettoIndex(
  engine: Engine,
  indexName: string,
  expression: string,
): Promise<AsyncDisposable> {
  await engine.query(`create perfetto index ${indexName} on ${expression}`);
  return {
    [Symbol.asyncDispose]: async () => {
      await engine.tryQuery(`drop perfetto index ${indexName}`);
    },
  };
}
