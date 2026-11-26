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

// Shared scalar types and SQL string helpers used by node generators.

import {assertUnreachable} from '../../base/assert';

export type SqlDialect = 'sql' | 'pipe';

export type FilterOp =
  | '='
  | '!='
  | '>'
  | '>='
  | '<'
  | '<='
  | 'LIKE'
  | 'NOT LIKE'
  | 'GLOB'
  | 'REGEXP'
  | 'IN'
  | 'BETWEEN'
  | 'IS NULL'
  | 'IS NOT NULL';

export type FilterConjunction = 'AND' | 'OR';

export interface FilterCondition {
  readonly column: string;
  readonly op: FilterOp;
  readonly value: string;
  readonly value2?: string;
}

export type PipelineAggFunc = 'COUNT' | 'SUM' | 'MIN' | 'MAX';

export type IntervalMergeKind = 'OVERLAPPING' | 'CONSECUTIVE';

export type EventsLastMode = 'KEEP_OPEN' | 'CLOSE_AT';

export type GraphTreeKind = 'BFS' | 'DFS' | 'DOMINATOR';

export type DataTableColumnType =
  'int' | 'double' | 'string' | 'timestamp' | 'duration' | 'boolean';

export interface DataTableColumn {
  readonly name: string;
  readonly type: DataTableColumnType;
}

export function isUnaryFilterOp(op: FilterOp): boolean {
  return op === 'IS NULL' || op === 'IS NOT NULL';
}

function isRawValue(value: string): boolean {
  if (value === '') return true;
  if (/^-?\d+(\.\d+)?([eE][+-]?\d+)?$/.test(value)) return true;
  if (/^null$/i.test(value)) return true;
  if (value.startsWith("'") || value.startsWith('"')) return true;
  if (value.startsWith('(')) return true;
  return false;
}

function quoteValue(value: string): string {
  return isRawValue(value) ? value : `'${value.replace(/'/g, "''")}'`;
}

function quoteStringPattern(value: string): string {
  if (value.startsWith("'") && value.endsWith("'") && value.length >= 2) {
    return value;
  }
  return `'${value.replace(/'/g, "''")}'`;
}

function formatInList(value: string): string {
  const trimmed = value.trim();
  if (trimmed.startsWith('(') && trimmed.endsWith(')')) {
    return trimmed;
  }
  const items = trimmed
    .split(',')
    .map((part) => part.trim())
    .filter((part) => part !== '')
    .map(quoteValue);
  return `(${items.join(', ')})`;
}

export function formatFilterConditions(
  conditions: readonly FilterCondition[],
  conjunction: FilterConjunction = 'AND',
): string {
  return conditions
    .filter((c) => c.column !== '')
    .map((c) => {
      if (isUnaryFilterOp(c.op)) {
        return `${c.column} ${c.op}`;
      }
      if (c.op === 'REGEXP') {
        return `${c.column} REGEXP ${quoteStringPattern(c.value)}`;
      }
      if (c.op === 'IN') {
        return `${c.column} IN ${formatInList(c.value)}`;
      }
      if (c.op === 'BETWEEN') {
        const hi = c.value2?.trim() ?? '';
        if (hi !== '') {
          return `${c.column} BETWEEN ${quoteValue(c.value.trim())} AND ${quoteValue(hi)}`;
        }
        return `${c.column} BETWEEN ${c.value}`;
      }
      return `${c.column} ${c.op} ${quoteValue(c.value)}`;
    })
    .join(` ${conjunction} `);
}

export function formatExtractArgExpr(
  argSetIdCol: string,
  argName: string,
): string {
  return `extract_arg(${argSetIdCol}, '${argName.replace(/'/g, "''")}')`;
}

function sqlCastType(type: DataTableColumnType): string {
  switch (type) {
    case 'int':
    case 'timestamp':
    case 'duration':
    case 'boolean':
      return 'INTEGER';
    case 'double':
      return 'REAL';
    case 'string':
      return 'TEXT';
    default:
      return assertUnreachable(type);
  }
}

function formatDataTableCell(raw: string, type: DataTableColumnType): string {
  if (type === 'string') {
    return raw === '' ? 'CAST(NULL AS TEXT)' : `'${raw.replaceAll("'", "''")}'`;
  }
  const trimmed = raw.trim();
  if (trimmed === '') return `CAST(NULL AS ${sqlCastType(type)})`;
  switch (type) {
    case 'int':
    case 'timestamp':
    case 'duration':
      return BigInt(trimmed).toString();
    case 'double': {
      const s = String(Number(trimmed));
      return /^[+-]?\d+$/.test(s) ? `${s}.0` : s;
    }
    case 'boolean':
      return /^(true|1)$/i.test(trimmed) ? '1' : '0';
    default:
      return assertUnreachable(type);
  }
}

/** `WITH _data(...) AS (VALUES ...) SELECT ...` for literal rows. */
export function formatDataTableSql(table: {
  readonly columns: readonly DataTableColumn[];
  readonly rows: readonly (readonly string[])[];
}): string {
  if (table.rows.length === 0) {
    const emptyCols = table.columns.map(
      (c) => `CAST(NULL AS ${sqlCastType(c.type)}) AS ${c.name}`,
    );
    return `SELECT\n  ${emptyCols.join(',\n  ')}\nWHERE 0`;
  }
  const colNames = table.columns.map((c) => c.name).join(', ');
  const valueTuples = table.rows.map((row) => {
    const cells = table.columns.map((col, idx) =>
      formatDataTableCell(row[idx] ?? '', col.type),
    );
    return `    (${cells.join(', ')})`;
  });
  return [
    `WITH _data(${colNames}) AS (`,
    '  VALUES',
    valueTuples.join(',\n'),
    ')',
    `SELECT ${colNames} FROM _data`,
  ].join('\n');
}
