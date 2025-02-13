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

import {isSqlColumnEqual, SqlColumn, sqlColumnId} from './sql_column';

// A filter which can be applied to the table.
export interface Filter {
  // Operation: it takes a list of column names and should return a valid SQL expression for this filter.
  op: (cols: string[]) => string;
  // Columns that the `op` should reference. The number of columns should match the number of interpolations in `op`.
  columns: SqlColumn[];
  // Returns a human-readable title for the filter. If not set, `op` will be used.
  // TODO(altimin): This probably should return m.Children, but currently Button expects its label to be string.
  getTitle?(): string;
}

// Returns a default string representation of the filter.
export function formatFilter(filter: Filter): string {
  return filter.op(filter.columns.map((c) => sqlColumnId(c)));
}

// Returns a human-readable title for the filter.
export function filterTitle(filter: Filter): string {
  if (filter.getTitle !== undefined) {
    return filter.getTitle();
  }
  return formatFilter(filter);
}

export function isFilterEqual(a: Filter, b: Filter): boolean {
  return (
    a.op === b.op &&
    a.columns.length === b.columns.length &&
    a.columns.every((c, i) => isSqlColumnEqual(c, b.columns[i]))
  );
}

export function areFiltersEqual(a: Filter[], b: Filter[]) {
  if (a.length !== b.length) return false;
  return a.every((f, i) => isFilterEqual(f, b[i]));
}
