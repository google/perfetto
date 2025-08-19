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

import {Row, SqlValue} from '../../../../trace_processor/query_result';
import {TableColumn} from '../table/table_column';
import {aggregationId} from './ids';

// Basic associative aggregation operations which can be pushed down to SQL.
export type BasicAggregation = {
  op: 'sum' | 'count' | 'min' | 'max';
  column: TableColumn;
};

// Higher-level aggregation operations, which are not associative and need
// to be resolved to associative operations first and have the final result computed
// based on the basic aggregations.
export type Aggregation =
  | {
      op: 'average';
      column: TableColumn;
    }
  | BasicAggregation;

// Some aggregations (e.g. average) are non-associative, so we need to expand them into basic
// associative aggregations and then compute the result from them.
export function expandAggregations(
  aggregations: ReadonlyArray<Aggregation>,
): BasicAggregation[] {
  const result: BasicAggregation[] = [];
  for (const agg of aggregations) {
    if (agg.op === 'average') {
      result.push({op: 'sum', column: agg.column});
      result.push({op: 'count', column: agg.column});
    } else {
      result.push(agg);
    }
  }
  return result;
}

// 'count' is intentionally excluded here, as it's special aggregation which is not associated
// with a column, so we just always show it, so we don't have to bother with figuring special
// UX for adding it.
export const AGGREGATIONS: Exclude<Aggregation['op'], 'count'>[] = [
  'sum',
  'min',
  'max',
  'average',
];

// We need to perform basic aggregation operations in JS.
export const basicAggregations: Record<
  BasicAggregation['op'],
  (a: SqlValue, b: SqlValue) => SqlValue
> = {
  sum: (a: SqlValue, b: SqlValue) => {
    if (a === null) return b;
    if (b === null) return a;
    if (typeof a === 'number' && typeof b === 'number') {
      return a + b;
    }
    if (typeof a === 'bigint' && typeof b === 'bigint') {
      return a + b;
    }
    return null;
  },
  count: (a: SqlValue, b: SqlValue) => {
    if (a === null) return b;
    if (b === null) return a;
    if (typeof a === 'number' && typeof b === 'number') {
      return a + b;
    }
    if (typeof a === 'bigint' && typeof b === 'bigint') {
      return a + b;
    }
    return null;
  },
  min: (a: SqlValue, b: SqlValue) => {
    if (a === null) return b;
    if (b === null) return a;
    if (a > b) return b;
    return a;
  },
  max: (a: SqlValue, b: SqlValue) => {
    if (a === null) return b;
    if (b === null) return a;
    if (a < b) return b;
    return a;
  },
};

function sqlValueAsNumber(value: SqlValue): number | null {
  if (typeof value === 'number') return value;
  if (typeof value === 'bigint') return Number(value);
  return null;
}

export function getAggregationValue(agg: Aggregation, row: Row): SqlValue {
  if (agg.op !== 'average') {
    return row[aggregationId(agg)];
  }
  const sum = sqlValueAsNumber(
    row[aggregationId({op: 'sum', column: agg.column})],
  );
  const count = sqlValueAsNumber(
    row[aggregationId({op: 'count', column: agg.column})],
  );
  if (sum === null || count === null) return null;
  return sum / count;
}
