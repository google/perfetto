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

import {stringifyJsonWithBigints} from '../../../base/json_utils';
import {SqlValue} from '../../../trace_processor/query_result';
import {
  DataGridDataSource,
  DataSourceResult,
  RowDef,
  Sorting,
  SortByColumn,
  DataGridModel,
  AggregateSpec,
  areAggregateArraysEqual,
  DataGridFilter,
} from './common';

export class InMemoryDataSource implements DataGridDataSource {
  private data: ReadonlyArray<RowDef> = [];
  private filteredSortedData: ReadonlyArray<RowDef> = [];
  private aggregateResults: RowDef = {};

  // Cached state for diffing
  private oldSorting: Sorting = {direction: 'UNSORTED'};
  private oldFilters: ReadonlyArray<DataGridFilter> = [];
  private aggregates?: ReadonlyArray<AggregateSpec>;

  constructor(data: ReadonlyArray<RowDef>) {
    this.data = data;
    this.filteredSortedData = data;
  }

  get rows(): DataSourceResult {
    return {
      rowOffset: 0,
      rows: this.filteredSortedData,
      totalRows: this.filteredSortedData.length,
      aggregates: this.aggregateResults,
    };
  }

  notifyUpdate({
    sorting = {direction: 'UNSORTED'},
    filters = [],
    aggregates,
  }: DataGridModel): void {
    if (
      !this.isSortByEqual(sorting, this.oldSorting) ||
      !this.areFiltersEqual(filters, this.oldFilters) ||
      !areAggregateArraysEqual(aggregates, this.aggregates)
    ) {
      this.oldSorting = sorting;
      this.oldFilters = filters;
      this.aggregates = aggregates;

      // Apply filters
      let result = this.applyFilters(this.data, filters);

      // Apply sorting
      result = this.applySorting(result, sorting);

      // Store the filtered and sorted data
      this.filteredSortedData = result;

      if (aggregates) {
        this.aggregateResults = this.calcAggregates(result, aggregates);
      }
    }
  }

  /**
   * Export all data with current filters/sorting applied.
   */
  async exportData(): Promise<readonly RowDef[]> {
    // Return all the filtered and sorted data
    return this.filteredSortedData;
  }

  private calcAggregates(
    results: ReadonlyArray<RowDef>,
    aggregates: ReadonlyArray<AggregateSpec>,
  ): RowDef {
    const result: RowDef = {};
    for (const aggregate of aggregates) {
      const {col, func} = aggregate;
      const values = results
        .map((row) => row[col])
        .filter((value) => value !== null);

      if (values.length === 0) {
        result[col] = null;
        continue;
      }

      switch (func) {
        case 'SUM':
          result[col] = values.reduce(
            (acc: number, val) => acc + (Number(val) || 0),
            0,
          );
          break;
        case 'AVG':
          result[col] =
            (values.reduce(
              (acc: number, val) => acc + (Number(val) || 0),
              0,
            ) as number) / values.length;
          break;
        case 'COUNT':
          result[col] = values.length;
          break;
        case 'MIN':
          result[col] = values.reduce(
            (acc, val) => (val < acc ? val : acc),
            values[0],
          );
          break;
        case 'MAX':
          result[col] = values.reduce(
            (acc, val) => (val > acc ? val : acc),
            values[0],
          );
          break;
        default:
          // Do nothing for unknown functions
          break;
      }
    }
    return result;
  }

  private isSortByEqual(a: Sorting, b: Sorting): boolean {
    if (a.direction === 'UNSORTED' && b.direction === 'UNSORTED') {
      return true;
    }

    if (a.direction !== 'UNSORTED' && b.direction !== 'UNSORTED') {
      const aColumn = a as SortByColumn;
      const bColumn = b as SortByColumn;
      return (
        aColumn.column === bColumn.column &&
        aColumn.direction === bColumn.direction
      );
    }

    return false;
  }

  // Helper function to compare arrays of filter definitions for equality.
  private areFiltersEqual(
    filtersA: ReadonlyArray<DataGridFilter>,
    filtersB: ReadonlyArray<DataGridFilter>,
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
    data: ReadonlyArray<RowDef>,
    filters: ReadonlyArray<DataGridFilter>,
  ): ReadonlyArray<RowDef> {
    if (filters.length === 0) {
      return data;
    }

    return data.filter((row) => {
      // Check if row passes all filters
      return filters.every((filter) => {
        const value = row[filter.column];

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
          case 'in':
            return filter.value.findIndex((v) => valuesEqual(v, value)) !== -1;
          case 'not in':
            return filter.value.findIndex((v) => valuesEqual(v, value)) === -1;
          default:
            return false;
        }
      });
    });
  }

  private applySorting(
    data: ReadonlyArray<RowDef>,
    sortBy: Sorting,
  ): ReadonlyArray<RowDef> {
    if (sortBy.direction === 'UNSORTED') {
      return data;
    }

    const sortColumn = (sortBy as SortByColumn).column;
    const sortDirection = (sortBy as SortByColumn).direction;

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
