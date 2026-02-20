// Copyright (C) 2026 The Android Open Source Project
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

import {assertUnreachable} from '../../../base/assert';
import {SqlValue} from '../../../trace_processor/query_result';
import {AggregateFunction, Filter} from './model';

/**
 * Converts a SqlValue to its SQL string representation.
 * Handles strings (with escaping), numbers, bigints, booleans, and null.
 */
export function sqlValue(value: SqlValue): string {
  if (value === null) {
    return 'NULL';
  } else if (typeof value === 'string') {
    return `'${value.replace(/'/g, "''")}'`;
  } else if (typeof value === 'number' || typeof value === 'bigint') {
    return value.toString();
  } else if (value instanceof Uint8Array) {
    // Convert Uint8Array to hex string for SQL BLOB representation
    const hex = Array.from(value)
      .map((b) => b.toString(16).padStart(2, '0'))
      .join('');
    return `X'${hex}'`;
  } else {
    assertUnreachable(value);
  }
}

/**
 * Converts a string to a valid SQL alias by wrapping in double quotes.
 * Escapes internal double quotes by doubling them (SQL standard).
 */
export function toAlias(id: string): string {
  return `"${id.replace(/"/g, '""')}"`;
}

/**
 * Generates a SQL condition that matches a single path, handling NULLs properly.
 * E.g., ['foo', null, 'bar'] with columns [a, b, c] becomes:
 * (a = 'foo' AND b IS NULL AND c = 'bar')
 */
export function sqlPathMatch(
  columns: readonly string[],
  path: readonly SqlValue[],
): string {
  const conditions = path.map((v, i) =>
    v === null ? `${columns[i]} IS NULL` : `${columns[i]} = ${sqlValue(v)}`,
  );
  return `(${conditions.join(' AND ')})`;
}

/**
 * Generates a SQL condition that excludes a single path, handling NULLs properly.
 * E.g., ['foo', null] with columns [a, b] becomes:
 * NOT (a = 'foo' AND b IS NULL)
 */
export function sqlPathNotMatch(
  columns: readonly string[],
  path: readonly SqlValue[],
): string {
  return `NOT ${sqlPathMatch(columns, path)}`;
}

/**
 * Builds an IN clause for paths, separating those with NULLs (which need OR conditions)
 * from those without (which can use efficient IN syntax).
 * Returns a SQL condition string.
 */
export function sqlPathsIn(
  columns: readonly string[],
  paths: readonly (readonly SqlValue[])[],
): string {
  const pathsWithNulls = paths.filter(pathHasNull);
  const pathsWithoutNulls = paths.filter((p) => !pathHasNull(p));

  const conditions: string[] = [];

  // Paths without NULLs can use efficient IN syntax
  if (pathsWithoutNulls.length > 0) {
    if (columns.length === 1) {
      // Single column: simple IN
      const values = pathsWithoutNulls.map((p) => sqlValue(p[0])).join(', ');
      conditions.push(`${columns[0]} IN (${values})`);
    } else {
      // Multiple columns: tuple IN
      const colTuple = columns.join(', ');
      const valueTuples = pathsWithoutNulls
        .map((path) => `(${path.map(sqlValue).join(', ')})`)
        .join(', ');
      conditions.push(`(${colTuple}) IN (${valueTuples})`);
    }
  }

  // Paths with NULLs need individual OR conditions
  for (const path of pathsWithNulls) {
    conditions.push(sqlPathMatch(columns, path));
  }

  if (conditions.length === 0) {
    return 'FALSE';
  } else if (conditions.length === 1) {
    return conditions[0];
  } else {
    return `(${conditions.join(' OR ')})`;
  }
}

/**
 * Builds a NOT IN clause for paths, separating those with NULLs (which need AND NOT conditions)
 * from those without (which can use efficient NOT IN syntax).
 * Returns a SQL condition string.
 */
export function sqlPathsNotIn(
  columns: readonly string[],
  paths: readonly (readonly SqlValue[])[],
): string {
  const pathsWithNulls = paths.filter(pathHasNull);
  const pathsWithoutNulls = paths.filter((p) => !pathHasNull(p));

  const conditions: string[] = [];

  // Paths without NULLs can use efficient NOT IN syntax
  if (pathsWithoutNulls.length > 0) {
    if (columns.length === 1) {
      // Single column: simple NOT IN
      const values = pathsWithoutNulls.map((p) => sqlValue(p[0])).join(', ');
      conditions.push(`${columns[0]} NOT IN (${values})`);
    } else {
      // Multiple columns: tuple NOT IN
      const colTuple = columns.join(', ');
      const valueTuples = pathsWithoutNulls
        .map((path) => `(${path.map(sqlValue).join(', ')})`)
        .join(', ');
      conditions.push(`(${colTuple}) NOT IN (${valueTuples})`);
    }
  }

  // Paths with NULLs need individual AND NOT conditions
  for (const path of pathsWithNulls) {
    conditions.push(sqlPathNotMatch(columns, path));
  }

  if (conditions.length === 0) {
    return 'TRUE';
  } else if (conditions.length === 1) {
    return conditions[0];
  } else {
    return conditions.join(' AND ');
  }
}

/**
 * Converts a filter to SQL using the resolved column expression.
 */
export function filterToSql(filter: Filter, sqlExpr: string): string {
  switch (filter.op) {
    case '=':
    case '!=':
    case '<':
    case '<=':
    case '>':
    case '>=':
      return `${sqlExpr} ${filter.op} ${sqlValue(filter.value)}`;
    case 'glob':
      return `${sqlExpr} GLOB ${sqlValue(filter.value)}`;
    case 'not glob':
      return `${sqlExpr} NOT GLOB ${sqlValue(filter.value)}`;
    case 'is null':
      return `${sqlExpr} IS NULL`;
    case 'is not null':
      return `${sqlExpr} IS NOT NULL`;
    case 'in':
      return `${sqlExpr} IN (${filter.value.map(sqlValue).join(', ')})`;
    case 'not in':
      return `${sqlExpr} NOT IN (${filter.value.map(sqlValue).join(', ')})`;
    default:
      assertUnreachable(filter);
  }
}

/**
 * Builds an aggregate expression string from function and field.
 * E.g., sqlAggregateExpr('SUM', 'dur') returns 'SUM(dur)'.
 */
export function sqlAggregateExpr(
  func: AggregateFunction,
  field: string,
): string {
  switch (func) {
    case 'ANY':
      return `MIN(${field})`;
    case 'COUNT_DISTINCT':
      return `COUNT(DISTINCT ${field})`;
    case 'SUM':
    case 'AVG':
    case 'MIN':
    case 'MAX':
      return `${func}(${field})`;
  }
}

function pathHasNull(path: readonly SqlValue[]): boolean {
  return path.some((v) => v === null);
}
