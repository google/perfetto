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

import {SqlValue} from '../../../../trace_processor/sql_utils';
import {LegacyTableColumn} from '../legacy_table/table_column';

export type Aggregation = {
  op: 'sum' | 'count' | 'min' | 'max';
  column: LegacyTableColumn;
};

// TODO(b:395565690): add support for "average".
//
// 'count' is intentionally excluded here, as it's special aggregation which is not associated
// with a column, so we just always show it, so we don't have to bother with figuring special
// UX for adding it.
export const AGGREGATIONS: ('sum' | 'min' | 'max')[] = ['sum', 'min', 'max'];

// We need to perform basic aggregation operations in JS.
export const basicAggregations: {
  [key: string]: (a: SqlValue, b: SqlValue) => SqlValue;
} = {
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
