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
import {assertUnreachable} from '../../../base/assert';
import {Row, SqlValue} from '../../../trace_processor/query_result';
import {
  DataSource,
  DataSourceModel,
  DataSourceRows,
  FlatModel,
  PivotModel,
} from './data_source';
import {AggregateFunction, Filter, GroupPath} from './model';

// Column shape from FlatModel
type FlatColumn = FlatModel['columns'][number];

export class InMemoryDataSource implements DataSource {
  private data: ReadonlyArray<Row> = [];
  private filteredSortedData: ReadonlyArray<Row> = [];
  private distinctValuesCache = new Map<string, ReadonlyArray<SqlValue>>();
  private parameterKeysCache = new Map<string, ReadonlyArray<string>>();
  private aggregateSummariesCache: Row = {};

  // Cached state for diffing (flat mode)
  private oldColumns?: readonly FlatColumn[];
  private oldFilters: ReadonlyArray<Filter> = [];
  private oldSort?: FlatModel['sort'];

  // Cached state for diffing (pivot mode)
  private oldPivotModel?: string;
  private pivotData: ReadonlyArray<Row> = [];

  constructor(data: ReadonlyArray<Row>) {
    this.data = data;
    this.filteredSortedData = data;
  }

  /**
   * Fetch rows for the current model state.
   */
  useRows(model: DataSourceModel): DataSourceRows {
    if (model.mode === 'flat') {
      return this.useFlatRows(model);
    } else if (model.mode === 'pivot') {
      return this.usePivotRows(model);
    }
    return {isPending: false};
  }

  private useFlatRows(model: FlatModel): DataSourceRows {
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

  private usePivotRows(model: PivotModel): DataSourceRows {
    // Use JSON serialization for change detection on the pivot model
    const modelKey = stringifyJsonWithBigints(model);
    if (modelKey !== this.oldPivotModel) {
      this.oldPivotModel = modelKey;
      this.aggregateSummariesCache = {};

      const filters = model.filters ?? [];
      const filtered = this.applyFilters(this.data, filters);

      if (model.groupDisplay === 'tree') {
        this.pivotData = this.buildPivotTree(filtered, model);
      } else {
        this.pivotData = this.buildPivotFlat(filtered, model);
      }
    }

    return {
      rowOffset: 0,
      rows: this.pivotData,
      totalRows: this.pivotData.length,
      isPending: false,
    };
  }

  /**
   * Flat pivot: simple GROUP BY — one row per unique group combination.
   */
  private buildPivotFlat(
    data: ReadonlyArray<Row>,
    model: PivotModel,
  ): ReadonlyArray<Row> {
    const groups = groupRows(data, model.groupBy);
    const rows: Row[] = [];

    for (const [, groupRows_] of groups) {
      const row: Row = {};
      // Add group-by columns using alias
      for (const col of model.groupBy) {
        row[col.alias] = groupRows_[0][col.field];
      }
      // Add aggregates
      for (const agg of model.aggregates) {
        if (agg.function === 'COUNT') {
          row[agg.alias] = groupRows_.length;
        } else {
          const values = groupRows_.map((r) => r[agg.field]);
          row[agg.alias] = computeAggregate(agg.function, values);
        }
      }
      rows.push(row);
    }

    // Apply sorting
    if (model.sort) {
      return this.applySorting(rows, model.sort.alias, model.sort.direction);
    }
    return rows;
  }

  /**
   * Tree pivot: hierarchical rows with __depth, __id, __parent_id,
   * __child_count, and __path_key metadata columns, matching the contract
   * of the SQL rollup tree data source.
   */
  private buildPivotTree(
    data: ReadonlyArray<Row>,
    model: PivotModel,
  ): ReadonlyArray<Row> {
    const groupByCols = model.groupBy;
    const aggregates = model.aggregates;

    // Build all tree nodes: one per depth level per unique group path.
    // Depth 0 = grand total, depth N = grouped by first N groupBy columns.
    interface TreeNode {
      depth: number;
      pathKey: string;
      parentPathKey: string;
      groupValues: SqlValue[]; // values for group columns at this level
      childRows: ReadonlyArray<Row>; // leaf rows under this node
    }

    const nodesByPathKey = new Map<string, TreeNode>();

    // Depth 0: grand total
    nodesByPathKey.set('', {
      depth: 0,
      pathKey: '',
      parentPathKey: '',
      groupValues: groupByCols.map(() => null),
      childRows: data,
    });

    // Build nodes for each depth level
    for (let depth = 1; depth <= groupByCols.length; depth++) {
      const colsAtLevel = groupByCols.slice(0, depth);
      const groups = groupRows(data, colsAtLevel);

      for (const [key, rows] of groups) {
        const values = colsAtLevel.map((c) => rows[0][c.field]);
        // Parent path key is the key for depth-1
        const parentValues = values.slice(0, depth - 1);
        const parentPathKey = depth === 1 ? '' : makeGroupKey(parentValues);

        // Full group values array (null-pad remaining columns)
        const groupValues = [
          ...values,
          ...Array(groupByCols.length - depth).fill(null),
        ];

        nodesByPathKey.set(key, {
          depth,
          pathKey: key,
          parentPathKey,
          groupValues,
          childRows: rows,
        });
      }
    }

    // Assign stable IDs and compute parent IDs and child counts
    const allNodes = Array.from(nodesByPathKey.values());
    allNodes.sort((a, b) => {
      if (a.depth !== b.depth) return a.depth - b.depth;
      return a.pathKey.localeCompare(b.pathKey);
    });

    const idByPathKey = new Map<string, number>();
    for (let i = 0; i < allNodes.length; i++) {
      idByPathKey.set(allNodes[i].pathKey, i + 1);
    }

    const childCounts = new Map<number, number>();
    for (const node of allNodes) {
      if (node.depth > 0) {
        const parentId = idByPathKey.get(node.parentPathKey)!;
        childCounts.set(parentId, (childCounts.get(parentId) ?? 0) + 1);
      }
    }

    // Determine which nodes are expanded using allowlist/denylist logic
    const useDenylist = model.collapsedGroups !== undefined;
    const expansionPaths = useDenylist
      ? model.collapsedGroups!
      : model.expandedGroups ?? [];

    const isExpanded = (node: TreeNode): boolean => {
      if (node.depth >= groupByCols.length) return false;
      const nodePath = node.groupValues.slice(0, node.depth);
      const matches = expansionPaths.some((p) => pathsEqual(p, nodePath));
      return useDenylist ? !matches : matches;
    };

    // DFS traversal to produce visible rows in tree order
    const result: Row[] = [];
    const sortAlias = model.sort?.alias;
    const sortDir = model.sort?.direction ?? 'DESC';

    const visit = (node: TreeNode) => {
      const nodeId = idByPathKey.get(node.pathKey)!;
      const parentId =
        node.depth === 0 ? null : idByPathKey.get(node.parentPathKey)!;

      const row: Row = {};
      // Group-by columns
      for (let i = 0; i < groupByCols.length; i++) {
        row[groupByCols[i].alias] = node.groupValues[i];
      }
      // Aggregates
      for (const agg of aggregates) {
        if (agg.function === 'COUNT') {
          row[agg.alias] = node.childRows.length;
        } else {
          const values = node.childRows.map((r) => r[agg.field]);
          row[agg.alias] = computeAggregate(agg.function, values);
        }
      }
      // Metadata columns
      row['__id'] = BigInt(nodeId);
      row['__parent_id'] = parentId === null ? null : BigInt(parentId);
      row['__depth'] = BigInt(node.depth);
      row['__child_count'] = BigInt(childCounts.get(nodeId) ?? 0);
      row['__path_key'] = node.pathKey;

      result.push(row);

      // Recurse into children if expanded
      if (isExpanded(node)) {
        let children = allNodes.filter(
          (n) => n.depth === node.depth + 1 && n.parentPathKey === node.pathKey,
        );

        // Sort children
        if (sortAlias) {
          children = [...children];
          children.sort((a, b) => {
            const rowA = this.buildAggRow(
              a.childRows,
              aggregates,
              groupByCols,
              a.groupValues,
            );
            const rowB = this.buildAggRow(
              b.childRows,
              aggregates,
              groupByCols,
              b.groupValues,
            );
            return compareSqlValues(rowA[sortAlias], rowB[sortAlias], sortDir);
          });
        }

        for (const child of children) {
          visit(child);
        }
      }
    };

    // Start from depth 0 (grand total) — but the SQL version starts from
    // minDepth which is typically 0. We include the root node.
    const rootNode = nodesByPathKey.get('')!;
    visit(rootNode);

    return result;
  }

  private buildAggRow(
    rows: ReadonlyArray<Row>,
    aggregates: PivotModel['aggregates'],
    groupByCols: PivotModel['groupBy'],
    groupValues: SqlValue[],
  ): Row {
    const row: Row = {};
    for (let i = 0; i < groupByCols.length; i++) {
      row[groupByCols[i].alias] = groupValues[i];
    }
    for (const agg of aggregates) {
      if (agg.function === 'COUNT') {
        row[agg.alias] = rows.length;
      } else {
        const values = rows.map((r) => r[agg.field]);
        row[agg.alias] = computeAggregate(agg.function, values);
      }
    }
    return row;
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

/**
 * Group rows by the given columns, returning a Map from group key to rows.
 */
function groupRows(
  data: ReadonlyArray<Row>,
  groupBy: readonly {readonly field: string; readonly alias: string}[],
): Map<string, Row[]> {
  const groups = new Map<string, Row[]>();
  for (const row of data) {
    const values = groupBy.map((col) => row[col.field]);
    const key = makeGroupKey(values);
    let group = groups.get(key);
    if (!group) {
      group = [];
      groups.set(key, group);
    }
    group.push(row);
  }
  return groups;
}

/**
 * Build a stable string key from an array of group column values.
 */
function makeGroupKey(values: SqlValue[]): string {
  return values
    .map((v) => {
      if (v === null) return '\0NULL';
      if (typeof v === 'bigint') return `\0BI:${v}`;
      return String(v);
    })
    .join('\0SEP');
}

/**
 * Compare two GroupPaths for equality.
 */
function pathsEqual(a: GroupPath, b: SqlValue[]): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (!valuesEqual(a[i], b[i])) return false;
  }
  return true;
}

/**
 * Compute an aggregate function over an array of values.
 */
function computeAggregate(fn: AggregateFunction, values: SqlValue[]): SqlValue {
  const nums = values.filter(isNumeric).map(Number);
  switch (fn) {
    case 'SUM':
      return nums.length === 0 ? null : nums.reduce((a, b) => a + b, 0);
    case 'AVG':
      return nums.length === 0
        ? null
        : nums.reduce((a, b) => a + b, 0) / nums.length;
    case 'MIN':
      return nums.length === 0 ? null : Math.min(...nums);
    case 'MAX':
      return nums.length === 0 ? null : Math.max(...nums);
    case 'ANY':
      return values.length === 0 ? null : values[0];
    case 'COUNT_DISTINCT': {
      const unique = new Set(
        values.map((v) => (v === null ? '\0' : String(v))),
      );
      return unique.size;
    }
    case 'P25':
      return percentile(nums, 25);
    case 'P50':
      return percentile(nums, 50);
    case 'P75':
      return percentile(nums, 75);
    case 'P90':
      return percentile(nums, 90);
    case 'P95':
      return percentile(nums, 95);
    case 'P99':
      return percentile(nums, 99);
    default:
      return null;
  }
}

function percentile(sorted: number[], p: number): SqlValue {
  if (sorted.length === 0) return null;
  const s = [...sorted].sort((a, b) => a - b);
  const idx = (p / 100) * (s.length - 1);
  const lo = Math.floor(idx);
  const hi = Math.ceil(idx);
  if (lo === hi) return s[lo];
  return s[lo] + (s[hi] - s[lo]) * (idx - lo);
}

/**
 * Compare two SqlValues for sorting purposes.
 */
function compareSqlValues(
  a: SqlValue,
  b: SqlValue,
  direction: 'ASC' | 'DESC',
): number {
  if (a === null && b === null) return 0;
  if (a === null) return direction === 'ASC' ? -1 : 1;
  if (b === null) return direction === 'ASC' ? 1 : -1;

  let cmp = 0;
  if (typeof a === 'number' && typeof b === 'number') {
    cmp = a - b;
  } else if (typeof a === 'bigint' && typeof b === 'bigint') {
    cmp = Number(a - b);
  } else {
    cmp = String(a).localeCompare(String(b));
  }
  return direction === 'ASC' ? cmp : -cmp;
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
