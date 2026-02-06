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

import {QueryResult} from '../../../base/query_slot';
import {stringifyJsonWithBigints} from '../../../base/json_utils';
import {assertUnreachable} from '../../../base/logging';
import {Row, SqlValue} from '../../../trace_processor/query_result';
import {
  DataSource,
  DataSourceModel,
  DataSourceRows,
  FlatModel,
} from './data_source';
import {Filter} from './model';

// Column shape from FlatModel
type FlatColumn = FlatModel['columns'][number];

export class InMemoryDataSource implements DataSource {
  private data: ReadonlyArray<Row> = [];
  private filteredSortedData: ReadonlyArray<Row> = [];
  private distinctValuesCache = new Map<string, ReadonlyArray<SqlValue>>();
  private parameterKeysCache = new Map<string, ReadonlyArray<string>>();
  private aggregateSummariesCache: Row = {};

  // Cached state for diffing
  private oldColumns?: readonly FlatColumn[];
  private oldFilters: ReadonlyArray<Filter> = [];
  private oldSort?: FlatModel['sort'];

  constructor(data: ReadonlyArray<Row>) {
    this.data = data;
    this.filteredSortedData = data;
  }

  /**
   * Fetch rows for the current model state.
   */
  useRows(model: DataSourceModel): DataSourceRows {
    // Only support flat mode
    if (model.mode !== 'flat') {
      return {isPending: false};
    }

    const columns = model.columns;
    const filters = model.filters ?? [];
    const sort = model.sort;

    if (
      !this.areColumnsEqual(columns, this.oldColumns) ||
      !this.areFiltersEqual(filters, this.oldFilters) ||
      !this.isSortEqual(sort, this.oldSort)
    ) {
      this.oldColumns = columns;
      this.oldFilters = filters;
      this.oldSort = sort;

      // Clear aggregate summaries cache
      this.aggregateSummariesCache = {};

      let result = this.applyFilters(this.data, filters);

      // Project columns to use aliases as keys (for consistency with SQL data source)
      result = this.projectColumns(result, columns);

      // Apply sorting from model
      if (sort) {
        result = this.applySorting(result, sort.alias, sort.direction);
      }

      // Store the filtered and sorted data
      this.filteredSortedData = result;
    }

    return {
      rowOffset: 0,
      rows: this.filteredSortedData,
      totalRows: this.filteredSortedData.length,
      isPending: false,
    };
  }

  /**
   * Fetch distinct values for a column.
   */
  useDistinctValues(
    column: string | undefined,
  ): QueryResult<readonly SqlValue[]> {
    if (column === undefined) {
      return {data: undefined, isPending: false, isFresh: true};
    }

    if (!this.distinctValuesCache.has(column)) {
      // Compute distinct values from base data (not filtered)
      const uniqueValues = new Set<SqlValue>();
      for (const row of this.data) {
        uniqueValues.add(row[column]);
      }

      // Sort with null-aware comparison
      const sorted = Array.from(uniqueValues).sort((a, b) => {
        // Nulls come first
        if (a === null && b === null) return 0;
        if (a === null) return -1;
        if (b === null) return 1;

        // Type-specific sorting
        if (typeof a === 'number' && typeof b === 'number') {
          return a - b;
        }
        if (typeof a === 'bigint' && typeof b === 'bigint') {
          return Number(a - b);
        }
        if (typeof a === 'string' && typeof b === 'string') {
          return a.localeCompare(b);
        }

        // Default: convert to string and compare
        return String(a).localeCompare(String(b));
      });

      this.distinctValuesCache.set(column, sorted);
    }

    return {
      data: this.distinctValuesCache.get(column),
      isPending: false,
      isFresh: true,
    };
  }

  /**
   * Fetch parameter keys for a parameterized column prefix.
   */
  useParameterKeys(prefix: string | undefined): QueryResult<readonly string[]> {
    if (prefix === undefined) {
      return {data: undefined, isPending: false, isFresh: true};
    }

    if (!this.parameterKeysCache.has(prefix)) {
      // Find all keys that match the prefix pattern (e.g., "skills.typescript" for prefix "skills")
      const uniqueKeys = new Set<string>();
      const prefixWithDot = prefix + '.';

      for (const row of this.data) {
        for (const key of Object.keys(row)) {
          if (key.startsWith(prefixWithDot)) {
            // Extract the parameter key (everything after the prefix)
            const paramKey = key.slice(prefixWithDot.length);
            // Only add top-level keys (no further dots)
            if (!paramKey.includes('.')) {
              uniqueKeys.add(paramKey);
            }
          }
        }
      }

      // Sort alphabetically
      const sorted = Array.from(uniqueKeys).sort((a, b) => a.localeCompare(b));

      this.parameterKeysCache.set(prefix, sorted);
    }

    return {
      data: this.parameterKeysCache.get(prefix),
      isPending: false,
      isFresh: true,
    };
  }

  /**
   * Fetch aggregate summaries (aggregates across all filtered rows).
   */
  useAggregateSummaries(_model: DataSourceModel): QueryResult<Row> {
    // Aggregates are computed in useRows, just return the cache
    const data =
      Object.keys(this.aggregateSummariesCache).length > 0
        ? this.aggregateSummariesCache
        : undefined;

    return {
      data,
      isPending: false,
      isFresh: true,
    };
  }

  /**
   * Export all data with current filters/sorting applied (no pagination).
   */
  async exportData(_model: DataSourceModel): Promise<readonly Row[]> {
    // Return all the filtered and sorted data
    return this.filteredSortedData;
  }

  /**
   * Compare columns for equality.
   */
  private areColumnsEqual(
    a: readonly FlatColumn[] | undefined,
    b: readonly FlatColumn[] | undefined,
  ): boolean {
    if (a === b) return true;
    if (!a || !b) return false;
    if (a.length !== b.length) return false;

    return a.every((colA, i) => {
      const colB = b[i];
      return colA.alias === colB.alias && colA.field === colB.field;
    });
  }

  /**
   * Compare sort configurations for equality.
   */
  private isSortEqual(a: FlatModel['sort'], b: FlatModel['sort']): boolean {
    if (a === b) return true;
    if (!a || !b) return false;
    return a.alias === b.alias && a.direction === b.direction;
  }

  /**
   * Project rows to use column aliases as keys instead of field names.
   * This ensures consistency with SQLDataSource which uses column IDs as SQL aliases.
   */
  private projectColumns(
    data: ReadonlyArray<Row>,
    columns: ReadonlyArray<FlatColumn>,
  ): ReadonlyArray<Row> {
    return data.map((row) => {
      const projectedRow: Row = {};
      for (const col of columns) {
        // Map field value to alias key
        projectedRow[col.alias] = row[col.field];
      }
      return projectedRow;
    });
  }

  // Helper function to compare arrays of filter definitions for equality.
  private areFiltersEqual(
    filtersA: ReadonlyArray<Filter>,
    filtersB: ReadonlyArray<Filter>,
  ): boolean {
    if (filtersA.length !== filtersB.length) return false;

    // Compare each filter
    return filtersA.every((filterA, index) => {
      const filterB = filtersB[index];
      return (
        stringifyJsonWithBigints(filterA) === stringifyJsonWithBigints(filterB)
      );
    });
  }

  private applyFilters(
    data: ReadonlyArray<Row>,
    filters: ReadonlyArray<Filter>,
  ): ReadonlyArray<Row> {
    if (filters.length === 0) {
      return data;
    }

    return data.filter((row) => {
      // Check if row passes all filters
      return filters.every((filter) => {
        const value = row[filter.field];

        switch (filter.op) {
          case '=':
            return valuesEqual(value, filter.value);
          case '!=':
            return !valuesEqual(value, filter.value);
          case '<':
            return compareNumeric(value, filter.value) < 0;
          case '<=':
            return compareNumeric(value, filter.value) <= 0;
          case '>':
            return compareNumeric(value, filter.value) > 0;
          case '>=':
            return compareNumeric(value, filter.value) >= 0;
          case 'is null':
            return value === null;
          case 'is not null':
            return value !== null;
          case 'glob':
            if (typeof value === 'string' && typeof filter.value === 'string') {
              // Simple glob matching - convert glob to regex
              const regexPattern = filter.value
                .replace(/\*/g, '.*')
                .replace(/\?/g, '.')
                .replace(/\[!([^\]]+)\]/g, '[^$1]');
              const regex = new RegExp(`^${regexPattern}$`);
              return regex.test(value);
            }
            return false;
          case 'not glob':
            if (typeof value === 'string' && typeof filter.value === 'string') {
              // Simple glob matching - convert glob to regex and negate
              const regexPattern = filter.value
                .replace(/\*/g, '.*')
                .replace(/\?/g, '.')
                .replace(/\[!([^\]]+)\]/g, '[^$1]');
              const regex = new RegExp(`^${regexPattern}$`);
              return !regex.test(value);
            }
            return false;
          case 'in':
            return filter.value.findIndex((v) => valuesEqual(v, value)) !== -1;
          case 'not in':
            return filter.value.findIndex((v) => valuesEqual(v, value)) === -1;
          default:
            assertUnreachable(filter);
        }
      });
    });
  }

  private applySorting(
    data: ReadonlyArray<Row>,
    sortColumn: string,
    sortDirection: 'ASC' | 'DESC',
  ): ReadonlyArray<Row> {
    return [...data].sort((a, b) => {
      const valueA = a[sortColumn];
      const valueB = b[sortColumn];

      // Handle null values - they come first in ascending, last in descending
      if (valueA === null && valueB === null) return 0;
      if (valueA === null) return sortDirection === 'ASC' ? -1 : 1;
      if (valueB === null) return sortDirection === 'ASC' ? 1 : -1;

      if (typeof valueA === 'number' && typeof valueB === 'number') {
        return sortDirection === 'ASC' ? valueA - valueB : valueB - valueA;
      }

      if (typeof valueA === 'bigint' && typeof valueB === 'bigint') {
        return sortDirection === 'ASC'
          ? Number(valueA - valueB)
          : Number(valueB - valueA);
      }

      if (typeof valueA === 'string' && typeof valueB === 'string') {
        return sortDirection === 'ASC'
          ? valueA.localeCompare(valueB)
          : valueB.localeCompare(valueA);
      }

      if (valueA instanceof Uint8Array && valueB instanceof Uint8Array) {
        // Compare by length for Uint8Arrays
        return sortDirection === 'ASC'
          ? valueA.length - valueB.length
          : valueB.length - valueA.length;
      }

      // Default comparison using string conversion
      const strA = String(valueA);
      const strB = String(valueB);
      return sortDirection === 'ASC'
        ? strA.localeCompare(strB)
        : strB.localeCompare(strA);
    });
  }
}

// Compare values, using a special deep comparison for Uint8Arrays.
function valuesEqual(a: SqlValue, b: SqlValue): boolean {
  if (a === b) {
    return true;
  }

  if (a instanceof Uint8Array && b instanceof Uint8Array) {
    if (a.length !== b.length) {
      return false;
    }

    for (let i = 0; i < a.length; i++) {
      if (a[i] !== b[i]) {
        return false;
      }
    }

    return true;
  }

  return false;
}

function isNumeric(value: SqlValue): value is number | bigint {
  return typeof value === 'number' || typeof value === 'bigint';
}

/**
 * Compare two numeric values (number or bigint).
 *
 * @returns Returns > 0 if a > b, < 0 if a < b, 0 if a == b.
 */
function compareNumeric(a: SqlValue, b: SqlValue): number {
  // Handle the null cases - null is always considered smaller than a numerical
  // value to match sqlite.
  if (a === null && b === null) return 0;
  if (a === null) return -1;
  if (b === null) return 1;

  if (!isNumeric(a) || !isNumeric(b)) {
    throw new Error('Cannot compare non-numeric values');
  }

  if (typeof a === 'number' && typeof b === 'number') {
    return a - b;
  } else if (typeof a === 'bigint' && typeof b === 'bigint') {
    return Number(a - b);
  } else {
    // One is a number and the other is a bigint. We've lost precision anyway,
    // so just convert both to numbers.
    return Number(a) - Number(b);
  }
}
