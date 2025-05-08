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

import m from 'mithril';
import {Button, ButtonBar, ButtonVariant} from '../../../../widgets/button';
import {Intent} from '../../../../widgets/common';
import {isSqlColumnEqual, SqlColumn, sqlColumnId} from './sql_column';
import {
  SqlValue,
  sqlValueToSqliteString,
} from '../../../../trace_processor/sql_utils';

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

// A class representing a set of filters. As it's common for multiple components to share the same set of filters (e.g.
// table viewer and associated charts), this class allows sharing the same set of filters between multiple components
// and them being notified when the filters change.
export class Filters {
  private filters: Filter[] = [];
  // Use WeakRef to allow observers to be reclaimed.
  private observers: (() => void)[] = [];

  constructor(filters: Filter[] = []) {
    this.filters = [...filters];
  }

  addFilter(filter: Filter) {
    this.filters.push(filter);
    this.notify();
  }

  addFilters(filter: ReadonlyArray<Filter>) {
    this.filters.push(...filter);
    this.notify();
  }

  removeFilter(filter: Filter) {
    const idx = this.filters.findIndex((f) => isFilterEqual(f, filter));
    if (idx === -1) throw new Error('Filter not found');
    this.filters.splice(idx, 1);
    this.notify();
  }

  setFilters(filters: ReadonlyArray<Filter>) {
    this.filters = [...filters];
    this.notify();
  }

  clear() {
    this.setFilters([]);
  }

  get(): Filter[] {
    return this.filters;
  }

  addObserver(observer: () => void) {
    this.observers.push(observer);
  }

  private notify() {
    this.observers.forEach((observer) => observer());
  }
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

export function areFiltersEqual(
  a: ReadonlyArray<Filter>,
  b: ReadonlyArray<Filter>,
) {
  if (a.length !== b.length) return false;
  return a.every((f, i) => isFilterEqual(f, b[i]));
}

export function renderFilters(filters: Filters): m.Children {
  return m(
    ButtonBar,
    filters.get().map((filter) =>
      m(Button, {
        label: filterTitle(filter),
        icon: 'close',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        onclick: () => filters.removeFilter(filter),
      }),
    ),
  );
}

export class StandardFilters {
  static valueEquals(col: SqlColumn, value: SqlValue): Filter {
    if (value === null) {
      return {
        columns: [col],
        op: (cols) => `${cols[0]} IS NULL`,
      };
    }
    return {
      columns: [col],
      op: (cols) => `${cols[0]} = ${sqlValueToSqliteString(value)}`,
    };
  }

  static valueNotEquals(col: SqlColumn, value: SqlValue): Filter {
    if (value === null) {
      return {
        columns: [col],
        op: (cols) => `${cols[0]} IS NOT NULL`,
      };
    }
    return {
      columns: [col],
      op: (cols) => `${cols[0]} != ${sqlValueToSqliteString(value)}`,
    };
  }

  static valueIsOneOf(col: SqlColumn, values: SqlValue[]): Filter {
    if (values.length === 1) return StandardFilters.valueEquals(col, values[0]);
    if (values.length === 0) {
      return {
        columns: [],
        op: () => 'FALSE',
      };
    }
    return {
      op: (cols) =>
        `${cols[0]} IN (${values.map(sqlValueToSqliteString).join(', ')})`,
      columns: [col],
    };
  }
}
