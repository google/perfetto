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

import {assertUnreachable} from '../../../base/logging';
import {UseQueryResult} from '../../../trace_processor/query_cache';
import {Row, SqlValue} from '../../../trace_processor/query_result';
import {DataSource, DataSourceModel, PivotRollups} from './data_source';
import {AggregateColumn, Column, Filter, Pivot} from './model';

export class InMemoryDataSource implements DataSource {
  private data: ReadonlyArray<Row> = [];
  private distinctValuesCache = new Map<string, ReadonlyArray<SqlValue>>();
  private parameterKeysCache = new Map<string, ReadonlyArray<string>>();

  // Last-result caches for expensive operations
  private rowsCache?: {
    key: string;
    result: {totalRows: number; offset: number; rows: Row[]};
  };
  private aggregateTotalsCache?: {
    key: string;
    result: ReadonlyMap<string, SqlValue>;
  };
  private pivotRollupsCache?: {
    key: string;
    result: PivotRollups;
  };

  constructor(data: ReadonlyArray<Row>) {
    this.data = data;
  }

  /**
   * Creates a cache key from model parameters.
   * Uses JSON.stringify for simplicity - works well for small objects.
   */
  private buildCacheKey(parts: Record<string, unknown>): string {
    return JSON.stringify(parts);
  }

  getRows(
    model: DataSourceModel,
  ): UseQueryResult<{totalRows: number; offset: number; rows: Row[]}> {
    const {columns, filters = [], pivot, pagination} = model;

    // Check cache first
    const cacheKey = this.buildCacheKey({columns, filters, pivot, pagination});
    if (this.rowsCache?.key === cacheKey) {
      return {result: this.rowsCache.result, isLoading: false};
    }

    // In pivot mode, separate filters into pre-pivot and post-pivot
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
    } else if (pivot?.drillDown) {
      // Drilldown mode: filter to show only rows matching the drillDown values
      result = this.applyDrillDown(result, pivot);
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

    const totalRows = result.length;
    const offset = pagination?.offset ?? 0;
    const limit = pagination?.limit ?? result.length;
    const rows = [...result.slice(offset, offset + limit)];

    const cachedResult = {totalRows, offset, rows};
    this.rowsCache = {key: cacheKey, result: cachedResult};

    return {
      result: cachedResult,
      isLoading: false,
    };
  }

  getDistinctValues(columnPath: string): UseQueryResult<readonly SqlValue[]> {
    // Check cache first
    const cached = this.distinctValuesCache.get(columnPath);
    if (cached) {
      return {result: cached, isLoading: false};
    }

    // Compute distinct values from base data (not filtered)
    const uniqueValues = new Set<SqlValue>();
    for (const row of this.data) {
      uniqueValues.add(row[columnPath]);
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

    this.distinctValuesCache.set(columnPath, sorted);
    return {result: sorted, isLoading: false};
  }

  getAggregateTotals(
    model: DataSourceModel,
  ): UseQueryResult<ReadonlyMap<string, SqlValue>> {
    const {columns, filters = [], pivot} = model;

    // Check cache first
    const cacheKey = this.buildCacheKey({columns, filters, pivot});
    if (this.aggregateTotalsCache?.key === cacheKey) {
      return {result: this.aggregateTotalsCache.result, isLoading: false};
    }

    // First get the filtered/pivoted data
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

    let result = this.applyFilters(this.data, prePivotFilters);

    const totals = new Map<string, SqlValue>();

    if (pivot && !pivot.drillDown) {
      result = this.applyPivoting(result, pivot);
      if (postPivotFilters.length > 0) {
        result = this.applyFilters(result, postPivotFilters);
      }
      // Compute aggregate totals across all filtered pivot rows
      this.computeAggregateTotalsFromData(result, pivot, totals);
    } else if (columns && !pivot) {
      // Non-pivot mode: compute column-level aggregations
      this.computeColumnAggregatesFromData(result, columns, totals);
    }

    this.aggregateTotalsCache = {key: cacheKey, result: totals};
    return {result: totals, isLoading: false};
  }

  getPivotRollups(model: DataSourceModel): UseQueryResult<PivotRollups> {
    const {filters = [], pivot} = model;

    // Only generate rollups for hierarchical pivot mode (2+ groupBy columns)
    if (!pivot || pivot.drillDown || pivot.groupBy.length < 2) {
      return {result: {byLevel: new Map()}, isLoading: false};
    }

    // Check cache
    const cacheKey = this.buildCacheKey({filters, pivot, _type: 'rollups'});
    if (this.pivotRollupsCache?.key === cacheKey) {
      return {result: this.pivotRollupsCache.result, isLoading: false};
    }

    // Apply pre-pivot filters
    const aggregates = pivot.aggregates ?? [];
    const aggregateFields = new Set(
      aggregates.map((a) => ('field' in a ? a.field : '__count__')),
    );
    const prePivotFilters = filters.filter(
      (f) => !aggregateFields.has(f.field),
    );
    const filteredData = this.applyFilters(this.data, prePivotFilters);

    // Build rollups for levels 0 to N-2
    const byLevel = new Map<number, Row[]>();
    const numLevels = pivot.groupBy.length;

    for (let level = 0; level < numLevels - 1; level++) {
      // Group by the first (level+1) columns
      const groupByFieldsForLevel = pivot.groupBy
        .slice(0, level + 1)
        .map(({field}) => field);
      const rollupRows = this.computeRollupForLevel(
        filteredData,
        groupByFieldsForLevel,
        aggregates,
      );
      byLevel.set(level, rollupRows);
    }

    const result: PivotRollups = {byLevel};
    this.pivotRollupsCache = {key: cacheKey, result};
    return {result, isLoading: false};
  }

  /**
   * Compute rollup rows for a specific level by grouping on the given fields.
   */
  private computeRollupForLevel(
    data: ReadonlyArray<Row>,
    groupByFields: readonly string[],
    aggregates: readonly AggregateColumn[],
  ): Row[] {
    const groups = new Map<string, Row[]>();

    for (const row of data) {
      const key = groupByFields.map((field) => row[field]).join('-');
      if (!groups.has(key)) {
        groups.set(key, []);
      }
      groups.get(key)!.push(row);
    }

    const result: Row[] = [];

    for (const group of groups.values()) {
      const newRow: Row = {};

      // Copy the groupBy field values
      for (const field of groupByFields) {
        newRow[field] = group[0][field];
      }

      // Compute aggregates
      for (const agg of aggregates) {
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
        }
      }

      result.push(newRow);
    }

    return result;
  }

  getParameterKeys(prefix: string): UseQueryResult<readonly string[]> {
    // Check cache first
    const cached = this.parameterKeysCache.get(prefix);
    if (cached) {
      return {result: cached, isLoading: false};
    }

    // Find all keys that match the prefix pattern
    const uniqueKeys = new Set<string>();
    const prefixWithDot = prefix + '.';

    for (const row of this.data) {
      for (const key of Object.keys(row)) {
        if (key.startsWith(prefixWithDot)) {
          const paramKey = key.slice(prefixWithDot.length);
          if (!paramKey.includes('.')) {
            uniqueKeys.add(paramKey);
          }
        }
      }
    }

    const sorted = Array.from(uniqueKeys).sort((a, b) => a.localeCompare(b));
    this.parameterKeysCache.set(prefix, sorted);
    return {result: sorted, isLoading: false};
  }

  /**
   * Export all data with current filters/sorting applied.
   */
  async exportData(): Promise<readonly Row[]> {
    // For export, return all data without pagination
    const result = this.getRows({});
    return result.result?.rows ?? [];
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
  private computeAggregateTotalsFromData(
    pivotedData: ReadonlyArray<Row>,
    pivot: Pivot,
    totals: Map<string, SqlValue>,
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
        totals.set(alias, null);
        continue;
      }

      switch (agg.function) {
        case 'SUM':
        case 'COUNT':
          // Sum up all the sums/counts
          totals.set(
            alias,
            values.reduce((acc: number, val) => acc + (Number(val) || 0), 0),
          );
          break;
        case 'AVG':
          // Average of averages (simple, not weighted)
          totals.set(
            alias,
            (values.reduce(
              (acc: number, val) => acc + (Number(val) || 0),
              0,
            ) as number) / values.length,
          );
          break;
        case 'MIN':
          totals.set(
            alias,
            values.reduce((acc, val) => (val < acc ? val : acc), values[0]),
          );
          break;
        case 'MAX':
          totals.set(
            alias,
            values.reduce((acc, val) => (val > acc ? val : acc), values[0]),
          );
          break;
        case 'ANY':
          // For ANY, just take the first value
          totals.set(alias, values[0]);
          break;
      }
    }
  }

  /**
   * Compute aggregates for columns with aggregation functions defined.
   * This is used in non-pivot mode when columns have individual aggregations.
   */
  private computeColumnAggregatesFromData(
    data: ReadonlyArray<Row>,
    columns: ReadonlyArray<Column>,
    totals: Map<string, SqlValue>,
  ): void {
    for (const col of columns) {
      if (!col.aggregate) continue;

      const values = data
        .map((row) => row[col.field])
        .filter((v) => v !== null);

      if (values.length === 0) {
        totals.set(col.field, null);
        continue;
      }

      switch (col.aggregate) {
        case 'SUM':
          totals.set(
            col.field,
            values.reduce((acc: number, val) => acc + (Number(val) || 0), 0),
          );
          break;
        case 'AVG':
          totals.set(
            col.field,
            (values.reduce(
              (acc: number, val) => acc + (Number(val) || 0),
              0,
            ) as number) / values.length,
          );
          break;
        case 'MIN':
          totals.set(
            col.field,
            values.reduce((acc, val) => (val < acc ? val : acc), values[0]),
          );
          break;
        case 'MAX':
          totals.set(
            col.field,
            values.reduce((acc, val) => (val > acc ? val : acc), values[0]),
          );
          break;
        case 'ANY':
          totals.set(col.field, values[0]);
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
