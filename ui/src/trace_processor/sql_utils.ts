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

import {SortDirection} from '../base/comparison_utils';
import {isString} from '../base/object_utils';
import {sqliteString} from '../base/string_utils';
import {Engine} from './engine';
import {SqlValue} from './query_result';

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
export function sqlValueToSqliteString(
  val: SqlValue | ReadonlyArray<SqlValue>,
): string {
  if (Array.isArray(val)) {
    return val.map((v) => sqlValueToSqliteString(v)).join(', ');
  }
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

function makeTempName(): string {
  // Generate a temporary name for a sql entity, which is guaranteed to be unique
  // within the current trace.
  return `__temp_${Math.random().toString(36).substring(2, 15)}`;
}

/**
 * Represents a disposable SQL entity, like a table or view.
 * In addition to being async disposable, it also has a name.
 */
export interface DisposableSqlEntity extends AsyncDisposable {
  readonly name: string;
}

async function createDisposableSqlEntity(
  engine: Engine,
  name: string,
  entityType: 'TABLE' | 'VIEW' | 'INDEX',
): Promise<DisposableSqlEntity> {
  return {
    name,
    [Symbol.asyncDispose]: async () => {
      await engine.tryQuery(`DROP ${entityType} IF EXISTS ${name}`);
    },
  };
}

type CreateTableArgs = {
  readonly engine: Engine;
  readonly as: string;
  readonly name?: string;
};

/**
 * Asynchronously creates a "perfetto" SQL table using the given engine and
 * returns a disposable object to handle its cleanup.
 *
 * @param args The arguments for creating the table.
 * @param args.engine The database engine to execute the query.
 * @param args.as The SQL expression to define the table.
 * @param args.name The name of the table to be created.
 * @returns An AsyncDisposable which drops the created table when disposed.
 */
export async function createPerfettoTable(
  args: CreateTableArgs,
): Promise<DisposableSqlEntity> {
  const {engine, as, name = makeTempName()} = args;
  await engine.query(`CREATE PERFETTO TABLE ${name} AS ${as}`);
  return createDisposableSqlEntity(engine, name, 'TABLE');
}

/**
 * Asynchronously creates a standard SQL table using the given engine and
 * returns a disposable object to handle its cleanup.
 *
 * @param args The arguments for creating the table.
 * @param args.engine The database engine to execute the query.
 * @param args.as The SQL expression to define the table.
 * @param args.name The name of the table to be created.
 * @returns An AsyncDisposable which drops the created table when disposed.
 */
export async function createTable(
  args: CreateTableArgs,
): Promise<DisposableSqlEntity> {
  const {engine, as, name = makeTempName()} = args;
  await engine.query(`CREATE TABLE ${name} AS ${as}`);
  return createDisposableSqlEntity(engine, name, 'TABLE');
}

type CreateViewArgs = {
  readonly engine: Engine;
  readonly as: string;
  readonly name?: string;
};

/**
 * Asynchronously creates a "perfetto" SQL view using the given engine and
 * returns a disposable object to handle its cleanup.
 *
 * @param args The arguments for creating the view.
 * @param args.engine The database engine to execute the query.
 * @param args.as The SQL expression to define the view.
 * @param args.name The name of the view to be created.
 * @returns An AsyncDisposable which drops the created view when disposed.
 */
export async function createPerfettoView(
  args: CreateViewArgs,
): Promise<DisposableSqlEntity> {
  const {engine, as, name = makeTempName()} = args;
  await engine.query(`CREATE PERFETTO VIEW ${name} AS ${as}`);
  return createDisposableSqlEntity(engine, name, 'VIEW');
}

/**
 * Asynchronously creates a standard SQL view using the given engine and
 * returns a disposable object to handle its cleanup.
 *
 * @param args The arguments for creating the view.
 * @param args.engine The database engine to execute the query.
 * @param args.as The SQL expression to define the view.
 * @param args.name The name of the view to be created.
 * @returns An AsyncDisposable which drops the created view when disposed.
 */
export async function createView(
  args: CreateViewArgs,
): Promise<DisposableSqlEntity> {
  const {engine, as, name = makeTempName()} = args;
  await engine.query(`CREATE VIEW ${name} AS ${as}`);
  return createDisposableSqlEntity(engine, name, 'VIEW');
}

type CreateIndexArgs = {
  readonly engine: Engine;
  readonly on: string;
  readonly name?: string;
};

/**
 * Asynchronously creates a "perfetto" SQL index using the given engine and
 * returns a disposable object to handle its cleanup.
 *
 * @param args The arguments for creating the index.
 * @param args.engine The database engine to execute the query.
 * @param args.on The table and columns to create the index on.
 * @param args.name The name of the index to be created.
 * @returns An AsyncDisposable which drops the created index when disposed.
 */
export async function createPerfettoIndex(
  args: CreateIndexArgs,
): Promise<DisposableSqlEntity> {
  const {engine, on, name = makeTempName()} = args;
  await engine.query(`CREATE PERFETTO INDEX ${name} ON ${on}`);
  return createDisposableSqlEntity(engine, name, 'INDEX');
}

/**
 * Asynchronously creates a standard SQL index using the given engine and
 * returns a disposable object to handle its cleanup.
 *
 * @param args The arguments for creating the index.
 * @param args.engine The database engine to execute the query.
 * @param args.on The table and columns to create the index on.
 * @param args.name The name of the index to be created.
 * @returns An AsyncDisposable which drops the created index when disposed.
 */
export async function createIndex(
  args: CreateIndexArgs,
): Promise<DisposableSqlEntity> {
  const {engine, on, name = makeTempName()} = args;
  await engine.query(`CREATE INDEX ${name} ON ${on}`);
  return createDisposableSqlEntity(engine, name, 'INDEX');
}

/**
 * Asynchronously creates a virtual SQL table using the given engine and returns
 * a disposable object to handle its cleanup.
 *
 * @param args The arguments for creating the virtual table.
 * @param args.engine The database engine to execute the query.
 * @param args.using The module to use for the virtual table.
 * @param args.name The name of the table to be created.
 * @returns An AsyncDisposable which drops the created table when disposed.
 *
 * @example
 * await using table = await createVirtualTable({
 *   engine,
 *   name: 'my_virtual_table',
 *   using: 'some_module',
 * });
 */
export async function createVirtualTable(args: {
  readonly engine: Engine;
  readonly name?: string;
  readonly using: string;
}): Promise<DisposableSqlEntity> {
  const {engine, using, name = makeTempName()} = args;
  await engine.query(`CREATE VIRTUAL TABLE ${name} USING ${using}`);
  return {
    name,
    [Symbol.asyncDispose]: async () => {
      await engine.tryQuery(`DROP TABLE IF EXISTS ${name}`);
    },
  };
}
