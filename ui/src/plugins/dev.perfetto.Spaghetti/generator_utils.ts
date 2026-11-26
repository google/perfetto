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

// Small string helpers shared by node SQL / PSN generators.

import type {PsnInput} from './node_types';

export {
  formatDataTableSql,
  formatExtractArgExpr,
  formatFilterConditions,
} from './sql_types';

export interface AggSpec {
  readonly func: string;
  readonly column: string;
  readonly alias: string;
}

/** `COUNT(*) AS n, SUM(dur) AS total` - COUNT always counts rows. */
export function formatAggs(aggs: readonly AggSpec[]): string {
  return aggs
    .map(
      (a) => `${a.func}(${a.func === 'COUNT' ? '*' : a.column}) AS ${a.alias}`,
    )
    .join(', ');
}

/** ` PER a, b` or '' (PSN). */
export function perClause(columns: readonly string[]): string {
  return columns.length > 0 ? ` PER ${columns.join(', ')}` : '';
}

/** `PARTITION BY a, b ` or '' (SQL window specs). */
export function partitionBy(columns: readonly string[]): string {
  const unique = [...new Set(columns)];
  return unique.length > 0 ? `PARTITION BY ${unique.join(', ')} ` : '';
}

/** Wrap a full SELECT statement as a parenthesized source. */
export function subquery(sql: string): string {
  const body = sql
    .split('\n')
    .map((line) => `  ${line}`)
    .join('\n');
  return `(\n${body}\n)`;
}

/**
 * Indent every line but the first, for embedding a (possibly multi-line)
 * source after an indented prefix, e.g. `  NODES ${indentTail(src, '  ')}`.
 */
export function indentTail(text: string, prefix: string): string {
  return text
    .split('\n')
    .map((line, i) => (i === 0 || line === '' ? line : `${prefix}${line}`))
    .join('\n');
}

/**
 * Wrap one or more PSN tree stages with `RENAME` before and after when the
 * input uses custom column names instead of `id` / `parent_id`.
 */
export function wrapTreeIdStages(
  idColumn: string,
  parentIdColumn: string,
  stages: readonly string[],
  renameBack = true,
): string[] {
  const renamesIn: string[] = [];
  const renamesOut: string[] = [];
  if (idColumn !== 'id') {
    renamesIn.push(`${idColumn} AS id`);
    renamesOut.push(`id AS ${idColumn}`);
  }
  if (parentIdColumn !== 'parent_id') {
    renamesIn.push(`${parentIdColumn} AS parent_id`);
    renamesOut.push(`parent_id AS ${parentIdColumn}`);
  }
  return [
    ...(renamesIn.length > 0 ? [`RENAME ${renamesIn.join(', ')}`] : []),
    ...stages,
    ...(renameBack && renamesOut.length > 0
      ? [`RENAME ${renamesOut.join(', ')}`]
      : []),
  ];
}

/** Normalize a secondary PSN input so its ID column is named `id`. */
export function idSource(input: PsnInput, idColumn: string): string {
  return idColumn === 'id'
    ? input.source
    : input.withStages([`SELECT ${idColumn} AS id`]);
}
