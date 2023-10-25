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

import {isString} from '../base/object_utils';
import {EngineProxy} from '../common/engine';
import {ColumnType, NUM} from '../common/query_result';
import {SortDirection} from '../common/state';

export interface OrderClause {
  fieldName: string;
  direction?: SortDirection;
}

export type CommonTableExpressions = {
  [key: string]: string|undefined
};

// Interface for defining constraints which can be passed to a SQL query.
export interface SQLConstraints {
  commonTableExpressions?: CommonTableExpressions;
  filters?: (undefined|string)[];
  joins?: (undefined|string)[];
  orderBy?: (undefined|string|OrderClause)[];
  groupBy?: (undefined|string)[];
  limit?: number;
}

function isDefined<T>(t: T|undefined): t is T {
  return t !== undefined;
}

export function constraintsToQueryPrefix(c: SQLConstraints): string {
  const ctes = Object.entries(c.commonTableExpressions ?? {})
                   .filter(([_, value]) => isDefined(value));
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
  if (c.limit) {
    result.push(`LIMIT ${c.limit}`);
  }
  return result.join('\n');
}

// Trace Processor returns number | null for NUM_NULL, while most of the UI
// code uses number | undefined. This functions provides a short-hand
// conversion.
// TODO(altimin): Support NUM_UNDEFINED as a first-class citizen.
export function fromNumNull(n: number|null): number|undefined {
  if (n === null) {
    return undefined;
  }
  return n;
}

export function sqlValueToString(val: ColumnType): string;
export function sqlValueToString(val?: ColumnType): string|undefined;
export function sqlValueToString(val?: ColumnType): string|undefined {
  if (val === undefined) return undefined;
  if (val instanceof Uint8Array) {
    return `<blob length=${val.length}>`;
  }
  if (val === null) {
    return 'NULL';
  }
  return val.toString();
}

export async function getTableRowCount(
    engine: EngineProxy, tableName: string): Promise<number|undefined> {
  const result =
      await engine.query(`SELECT COUNT() as count FROM ${tableName}`);
  if (result.numRows() === 0) {
    return undefined;
  }
  return result
      .firstRow({
        count: NUM,
      })
      .count;
}
