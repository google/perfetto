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
import {assertUnreachable} from '../../../base/logging';
import {Row, SqlValue} from '../../../trace_processor/query_result';
import {DataSource, DataSourceModel, DataSourceRows} from './data_source';
import {Column, Filter, Pivot} from './model';

export class InMemoryDataSource implements DataSource {
  private data: ReadonlyArray<Row> = [];
  private filteredSortedData: ReadonlyArray<Row> = [];
  private distinctValuesCache = new Map<string, ReadonlyArray<SqlValue>>();
  private parameterKeysCache = new Map<string, ReadonlyArray<string>>();
  private aggregateTotalsCache = new Map<string, SqlValue>();

  // Cached state for diffing
  private oldColumns?: readonly Column[];
  private oldFilters: ReadonlyArray<Filter> = [];
  private oldPivot?: Pivot;

  constructor(data: ReadonlyArray<Row>) {
    this.data = data;
    this.filteredSortedData = data;
  }

  get rows(): DataSourceRows {
    return {
      rowOffset: 0,
      rows: this.filteredSortedData,
      totalRows: this.filteredSortedData.length,
    };
  }

  get distinctValues(): ReadonlyMap<string, readonly SqlValue[]> | undefined {
    return this.distinctValuesCache.size > 0
      ? this.distinctValuesCache
      : undefined;
  }

  get parameterKeys(): ReadonlyMap<string, readonly string[]> | undefined {
    return this.parameterKeysCache.size > 0
      ? this.parameterKeysCache
      : undefined;
  }

  get aggregateTotals(): ReadonlyMap<string, SqlValue> | undefined {
    return this.aggregateTotalsCache.size > 0
      ? this.aggregateTotalsCache
      : undefined;
  }

  notify({
    columns,
    filters = [],
    pivot,
    distinctValuesColumns,
    parameterKeyColumns,
  }: DataSourceModel): void {
    if (
      !this.areColumnsEqual(columns, this.oldColumns) ||
      !this.areFiltersEqual(filters, this.oldFilters) ||
      !arePivotsEqual(pivot, this.oldPivot)
    ) {
      this.oldColumns = columns;
      this.oldFilters = filters;
      this.oldPivot = pivot;

      // Clear aggregate totals cache
      this.aggregateTotalsCache.clear();

      // In pivot mode, separate filters into pre-pivot and post-pivot
      // Post-pivot filters apply to aggregate columns
      const aggregates = pivot?.aggregates ?? [];
      const aggregateFields = new Set(
        aggregates.map((a) => ('field' in a ? a.field : '__count__')),
      );
      const prePivotFilters =
        pivot && !pivot.drillDown
          ? filters.filter((f) => !aggregateFields.has(f.field))
          : filters;
      const postPivotFilters =
        pivot && !pivot.drillDown
          ? filters.filter((f) => aggregateFields.has(f.field))
          : [];

      // Apply pre-pivot filters (on source data)
      let result = this.applyFilters(this.data, prePivotFilters);

      // Apply pivot (but not in drilldown mode - drilldown shows raw data)
      if (pivot && !pivot.drillDown) {
        result = this.applyPivoting(result, pivot);
        // Apply post-pivot filters (on aggregate results)
        if (postPivotFilters.length > 0) {
          result = this.applyFilters(result, postPivotFilters);
        }
        // Compute aggregate totals across all filtered pivot rows
        this.computeAggregateTotals(result, pivot);
      } else if (pivot?.drillDown) {
        // Drilldown mode: filter to show only rows matching the drillDown values
        result = this.applyDrillDown(result, pivot);
      } else if (columns) {
        // Non-pivot mode: compute column-level aggregations
        this.computeColumnAggregates(result, columns);
      }

      // Apply sorting - find sorted column from columns or pivot
      const sortedColumn = this.findSortedColumn(columns, pivot);
      if (sortedColumn) {
        result = this.applySorting(
          result,
          sortedColumn.field,
          sortedColumn.direction,
        );
      }

      // Store the filtered and sorted data
      this.filteredSortedData = result;
    }

    // Handle distinct values requests
    if (distinctValuesColumns) {
      for (const column of distinctValuesColumns) {
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
      }
    }

    // Handle parameter keys requests
    if (parameterKeyColumns) {
      for (const prefix of parameterKeyColumns) {
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
          const sorted = Array.from(uniqueKeys).sort((a, b) =>
            a.localeCompare(b),
          );

          this.parameterKeysCache.set(prefix, sorted);
        }
      }
    }
  }

  /**
   * Export all data with current filters/sorting applied.
   */
  async exportData(): Promise<readonly Row[]> {
    // Return all the filtered and sorted data
    return this.filteredSortedData;
  }

  /**
   * Compare columns for equality (including sort state).
   */
  private areColumnsEqual(
    a: readonly Column[] | undefined,
    b: readonly Column[] | undefined,
  ): boolean {
    if (a === b) return true;
    if (!a || !b) return false;
    if (a.length !== b.length) return false;

    return a.every((colA, i) => {
      const colB = b[i];
      return (
        colA.field === colB.field &&
        colA.sort === colB.sort &&
        colA.aggregate === colB.aggregate
      );
    });
  }

  /**
   * Find the column that has sorting applied.
   */
  private findSortedColumn(
    columns: readonly Column[] | undefined,
    pivot?: Pivot,
  ): {field: string; direction: 'ASC' | 'DESC'} | undefined {
    // Check pivot groupBy columns for sort
    if (pivot) {
      for (const col of pivot.groupBy) {
        if (typeof col !== 'string' && col.sort) {
          return {field: col.field, direction: col.sort};
        }
      }
      // Check pivot aggregates for sort
      for (const agg of pivot.aggregates ?? []) {
        if (agg.sort) {
          const field = 'field' in agg ? agg.field : '__count__';
          return {field, direction: agg.sort};
        }
      }
    }

    // Check regular columns for sort
    if (columns) {
      for (const col of columns) {
        if (col.sort) {
          return {field: col.field, direction: col.sort};
        }
      }
    }

    return undefined;
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

  private applyPivoting(
    data: ReadonlyArray<Row>,
    pivot: Pivot,
  ): ReadonlyArray<Row> {
    const groups = new Map<string, Row[]>();
    const groupByFields = pivot.groupBy.map(({field}) => field);

    for (const row of data) {
      const key = groupByFields.map((field) => row[field]).join('-');
      if (!groups.has(key)) {
        groups.set(key, []);
      }
      groups.get(key)!.push(row);
    }

    const result: Row[] = [];
    const aggregates = pivot.aggregates ?? [];

    for (const group of groups.values()) {
      const newRow: Row = {};
      for (const field of groupByFields) {
        newRow[field] = group[0][field];
      }
      for (const agg of aggregates) {
        // Determine the alias (field name in the result row)
        const alias =
          agg.function === 'COUNT'
            ? '__count__'
            : 'field' in agg
              ? agg.field
              : '__unknown__';

        if (agg.function === 'COUNT') {
          newRow[alias] = group.length;
          continue;
        }

        const aggField = 'field' in agg ? agg.field : null;
        if (!aggField) {
          newRow[alias] = null;
          continue;
        }

        const values = group
          .map((row) => row[aggField])
          .filter((v) => v !== null);
        if (values.length === 0) {
          newRow[alias] = null;
          continue;
        }
        switch (agg.function) {
          case 'SUM':
            newRow[alias] = values.reduce(
              (acc: number, val) => acc + (Number(val) || 0),
              0,
            );
            break;
          case 'AVG':
            newRow[alias] =
              (values.reduce(
                (acc: number, val) => acc + (Number(val) || 0),
                0,
              ) as number) / values.length;
            break;
          case 'MIN':
            newRow[alias] = values.reduce(
              (acc, val) => (val < acc ? val : acc),
              values[0],
            );
            break;
          case 'MAX':
            newRow[alias] = values.reduce(
              (acc, val) => (val > acc ? val : acc),
              values[0],
            );
            break;
          case 'ANY':
            newRow[alias] = values[0];
            break;
          default:
            break;
        }
      }
      result.push(newRow);
    }

    return result;
  }

  private applyDrillDown(
    data: ReadonlyArray<Row>,
    pivot: Pivot,
  ): ReadonlyArray<Row> {
    const drillDown = pivot.drillDown!;

    return data.filter((row) => {
      // Check if this row matches all the drillDown values
      return pivot.groupBy.every((col) => {
        const field = col.field;
        const drillDownValue = drillDown[field];
        const rowValue = row[field];
        return valuesEqual(rowValue, drillDownValue);
      });
    });
  }

  /**
   * Compute grand totals for each aggregate column across all pivot rows.
   * For SUM, we sum all values. For COUNT, we sum all counts.
   * For AVG, we compute the average of averages (weighted by count would be better but we don't have that info).
   * For MIN/MAX, we find the min/max across all groups.
   */
  private computeAggregateTotals(
    pivotedData: ReadonlyArray<Row>,
    pivot: Pivot,
  ): void {
    const aggregates = pivot.aggregates ?? [];
    for (const agg of aggregates) {
      const alias =
        agg.function === 'COUNT'
          ? '__count__'
          : 'field' in agg
            ? agg.field
            : '__unknown__';
      const values = pivotedData
        .map((row) => row[alias])
        .filter((v) => v !== null);

      if (values.length === 0) {
        this.aggregateTotalsCache.set(alias, null);
        continue;
      }

      switch (agg.function) {
        case 'SUM':
        case 'COUNT':
          // Sum up all the sums/counts
          this.aggregateTotalsCache.set(
            alias,
            values.reduce((acc: number, val) => acc + (Number(val) || 0), 0),
          );
          break;
        case 'AVG':
          // Average of averages (simple, not weighted)
          this.aggregateTotalsCache.set(
            alias,
            (values.reduce(
              (acc: number, val) => acc + (Number(val) || 0),
              0,
            ) as number) / values.length,
          );
          break;
        case 'MIN':
          this.aggregateTotalsCache.set(
            alias,
            values.reduce((acc, val) => (val < acc ? val : acc), values[0]),
          );
          break;
        case 'MAX':
          this.aggregateTotalsCache.set(
            alias,
            values.reduce((acc, val) => (val > acc ? val : acc), values[0]),
          );
          break;
        case 'ANY':
          // For ANY, just take the first value
          this.aggregateTotalsCache.set(alias, values[0]);
          break;
      }
    }
  }

  /**
   * Compute aggregates for columns with aggregation functions defined.
   * This is used in non-pivot mode when columns have individual aggregations.
   */
  private computeColumnAggregates(
    data: ReadonlyArray<Row>,
    columns: ReadonlyArray<Column>,
  ): void {
    for (const col of columns) {
      if (!col.aggregate) continue;

      const values = data
        .map((row) => row[col.field])
        .filter((v) => v !== null);

      if (values.length === 0) {
        this.aggregateTotalsCache.set(col.field, null);
        continue;
      }

      switch (col.aggregate) {
        case 'SUM':
          this.aggregateTotalsCache.set(
            col.field,
            values.reduce((acc: number, val) => acc + (Number(val) || 0), 0),
          );
          break;
        case 'AVG':
          this.aggregateTotalsCache.set(
            col.field,
            (values.reduce(
              (acc: number, val) => acc + (Number(val) || 0),
              0,
            ) as number) / values.length,
          );
          break;
        case 'MIN':
          this.aggregateTotalsCache.set(
            col.field,
            values.reduce((acc, val) => (val < acc ? val : acc), values[0]),
          );
          break;
        case 'MAX':
          this.aggregateTotalsCache.set(
            col.field,
            values.reduce((acc, val) => (val > acc ? val : acc), values[0]),
          );
          break;
        case 'ANY':
          this.aggregateTotalsCache.set(col.field, values[0]);
          break;
      }
    }
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

function arePivotsEqual(a?: Pivot, b?: Pivot): boolean {
  if (a === b) return true;
  if (a === undefined || b === undefined) return false;
  // Compare groupBy fields
  const aGroupBy = a.groupBy.map(({field}) => field).join(',');
  const bGroupBy = b.groupBy.map(({field}) => field).join(',');
  if (aGroupBy !== bGroupBy) return false;
  // Compare aggregates
  if (JSON.stringify(a.aggregates) !== JSON.stringify(b.aggregates)) {
    return false;
  }
  // Check drillDown equality
  if (a.drillDown === b.drillDown) return true;
  if (a.drillDown === undefined || b.drillDown === undefined) return false;
  if (JSON.stringify(a.drillDown) !== JSON.stringify(b.drillDown)) return false;
  return true;
}
